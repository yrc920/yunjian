#include "composerbridge.h"

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QSignalBlocker>
#include <QMimeData>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QUuid>

#include "usermgr.h"

namespace {
constexpr int UploadMaxSide = 1600; //上传压缩图的长边上限，点开大图时不至于模糊
constexpr int ThumbMaxWidth = 480; //气泡缩略图按 2x 屏幕准备，实际显示再缩到 220 宽
constexpr int ThumbMaxHeight = 360;
constexpr int PreviewMaxWidth = 160; //输入区里的显示尺寸，与图片像素尺寸分开
constexpr int PreviewMaxHeight = 100;
constexpr int ImageQuality = 86;
//表情图片的唯一格式号：Qt 会把相邻且格式完全相同的图片并成一个 fragment，
//TextEdit 只画这个 fragment 的第一张，连打两个同款表情就会少一个
constexpr int EmojiSerialProperty = QTextFormat::UserProperty + 40;

//带透明像素的图必须存 PNG，否则透明区域会变黑；不透明照片一律 JPEG，体积小很多
bool hasTransparency(const QImage &image)
{
    if (!image.hasAlphaChannel())
        return false;
    const QImage argb = image.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(line[x]) < 255)
                return true;
        }
    }
    return false;
}

//按长边等比缩放；图片本身更小时不放大，避免把缩略图再糊一层
QImage shrinkTo(const QImage &image, int max_width, int max_height)
{
    if (image.width() <= max_width && image.height() <= max_height)
        return image;
    return image.scaled(max_width, max_height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

QByteArray encodeImage(const QImage &image, QByteArray *format)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    if (hasTransparency(image)) {
        if (format)
            *format = "png";
        return image.save(&buffer, "PNG") ? bytes : QByteArray();
    }
    if (format)
        *format = "jpg";
    return image.save(&buffer, "JPG", ImageQuality) ? bytes : QByteArray();
}
}

ComposerBridge::ComposerBridge(QObject *parent) : QObject(parent)
{
    //待上传图按登录账号存放，登录时由 UserMgr::CleanPendingImages 清理上次退出留下的文件；
    //这里构造时还没有账号信息，不能直接拼路径
    //草稿防抖：连续输入时只在停下后写一次库
    _draftTimer.setSingleShot(true);
    _draftTimer.setInterval(600);
    connect(&_draftTimer, &QTimer::timeout, this, &ComposerBridge::saveDraft);

    //设置里清空历史时草稿也一起没了：内存缓存与当前文档都要清，否则切回去又冒出来
    connect(UserMgr::GetInstance().get(), &UserMgr::DraftsCleared, this,
            [this](qint64 threadId) {
                if (threadId == 0) {
                    _drafts.clear();
                    _attachment_drafts.clear();
                    if (!_attachments.isEmpty()) {
                        _attachments.clear();
                        emit attachmentsChanged();
                    }
                    if (_document) {
                        const QSignalBlocker blocker(_document);
                        _document->clear();
                    }
                    return;
                }
                _drafts.remove(threadId);
                _attachment_drafts.remove(threadId);
                if (threadId == _draftKey) {
                    if (!_attachments.isEmpty()) {
                        _attachments.clear();
                        emit attachmentsChanged();
                    }
                    if (_document) {
                        const QSignalBlocker blocker(_document);
                        _document->clear();
                    }
                }
            });
}

void ComposerBridge::saveDraft()
{
    if (_draftKey <= 0 || !_document)
        return;
    //空文档的 toHtml() 也是完整 HTML，必须先判断有没有实际内容；
    //否则会话列表会把"没有草稿"显示成"未寄出 · 草稿"
    const QString raw = _document->toPlainText();
    const bool empty = raw.trimmed().isEmpty()
        && !raw.contains(QChar::ObjectReplacementCharacter);
    UserMgr::GetInstance()->SetThreadDraft(_draftKey, empty ? QString() : _document->toHtml());
}

//发送成功：停掉防抖定时器并立即把草稿置空，列表预览马上回到最后一条消息
void ComposerBridge::clearDraft()
{
    _draftTimer.stop();
    _drafts.remove(_draftKey);
    if (_draftKey > 0)
        UserMgr::GetInstance()->SetThreadDraft(_draftKey, QString());
}

void ComposerBridge::refreshInlineContent()
{
    bool has = false;
    if (_document) {
        for (QTextBlock block = _document->begin(); block.isValid() && !has; block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
                    has = true;
                    break;
                }
            }
        }
    }
    if (has == _hasInlineContent)
        return;
    _hasInlineContent = has;
    emit inlineContentChanged();
}

void ComposerBridge::setError(const QString &message)
{
    _errorText = message;
    emit errorTextChanged();
}

QString ComposerBridge::imageKey(const QString &source)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha1).toHex());
}

//上传文件放在当前账号的 pending 目录，sendImage 复制走并生成缓存，整批成功后由 ChatBridge 删除
QString ComposerBridge::savePendingImage(const QByteArray &data, const QByteArray &suffix)
{
    const QString dir_path = UserMgr::PendingImageDir();
    if (dir_path.isEmpty())
        return {};
    QDir dir(dir_path);
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
        return {};
    const QString path = dir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QLatin1Char('.') + QString::fromLatin1(suffix));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        file.close();
        QFile::remove(path);
        return {};
    }
    return path;
}

int ComposerBridge::insertImage(QQuickTextDocument *document, int position, const QUrl &url)
{
    if (!url.isLocalFile()) {
        setError(tr("请拖入本地图片文件"));
        return position;
    }
    QImageReader reader(url.toLocalFile());
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && (size.width() > 1600 || size.height() > 1600))
        reader.setScaledSize(size.scaled(1600, 1600, Qt::KeepAspectRatio));
    return insertImageData(document, position, reader.read());
}

int ComposerBridge::insertImageData(QQuickTextDocument *document, int position, const QImage &image)
{
    if (!document || !document->textDocument())
        return position;
    if (image.isNull()) {
        setError(tr("无法读取图片；暂不支持文件发送"));
        return position;
    }

    //上传图保留长边 1600 的清晰度，供其他客户端下载和以后点开大图使用
    const QImage upload = shrinkTo(image, UploadMaxSide, UploadMaxSide);
    QByteArray upload_suffix;
    const QByteArray upload_bytes = encodeImage(upload, &upload_suffix);
    if (upload_bytes.isEmpty()) {
        setError(tr("图片压缩失败"));
        return position;
    }
    const QString upload_path = savePendingImage(upload_bytes, upload_suffix);
    if (upload_path.isEmpty()) {
        setError(tr("无法保存待发送图片"));
        return position;
    }

    //气泡只加载缩略图，滚动长会话时不会反复解码大图
    const QImage thumb = shrinkTo(upload, ThumbMaxWidth, ThumbMaxHeight);
    QByteArray thumb_suffix;
    const QByteArray thumb_bytes = encodeImage(thumb, &thumb_suffix);
    if (thumb_bytes.isEmpty()) {
        QFile::remove(upload_path);
        setError(tr("图片压缩失败"));
        return position;
    }
    const QString mime = thumb_suffix == "png" ? QStringLiteral("png") : QStringLiteral("jpeg");
    const QString source = QStringLiteral("data:image/%1;base64,%2")
        .arg(mime, QString::fromLatin1(thumb_bytes.toBase64()));

    //输入区按 160×100 显示，像素尺寸仍以缩略图为准，发送后气泡据此算宽高比
    const QSize display = thumb.size().scaled(PreviewMaxWidth, PreviewMaxHeight, Qt::KeepAspectRatio);
    QTextImageFormat format;
    format.setName(source);
    format.setWidth(display.width());
    format.setHeight(display.height());
    QTextCursor cursor(document->textDocument());
    cursor.setPosition(qBound(0, position, document->textDocument()->characterCount() - 1));
    cursor.insertImage(format);
    _images.insert(imageKey(source), InsertedImage{upload_path, thumb.size()});
    refreshInlineContent();
    setError({});
    return cursor.position();
}

//拖入/粘贴时按扩展名分流；只接受真实存在的本地文件，目录和空文件直接忽略
QString ComposerBridge::fileKind(const QUrl &url) const
{
    if (!url.isLocalFile())
        return {};
    const QFileInfo info(url.toLocalFile());
    if (!info.isFile() || info.size() <= 0)
        return {};

    static const QStringList images = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("webp")
    };
    static const QStringList videos = {
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("avi"), QStringLiteral("webm")
    };
    const QString suffix = info.suffix().toLower();
    if (images.contains(suffix))
        return QStringLiteral("image");
    if (videos.contains(suffix))
        return QStringLiteral("video");
    return QStringLiteral("file");
}

QVariantList ComposerBridge::clipboardFiles() const
{
    QVariantList result;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasUrls())
        return result;
    for (const QUrl &url : mime->urls()) {
        const QString kind = fileKind(url);
        if (kind.isEmpty())
            continue;
        result.append(QVariantMap{{QStringLiteral("url"), url.toString()},
                                  {QStringLiteral("kind"), kind}});
    }
    return result;
}

int ComposerBridge::paste(QQuickTextDocument *document, int position, int selectionEnd)
{    if (!document || !document->textDocument())
        return position;
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime)
        return position;
    QTextCursor cursor(document->textDocument());
    const int end = document->textDocument()->characterCount() - 1;
    cursor.setPosition(qBound(0, position, end));
    cursor.setPosition(qBound(0, selectionEnd, end), QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    position = cursor.position();
    if (mime->hasUrls()) {
        for (const QUrl &url : mime->urls())
            position = insertImage(document, position, url);
        return position;
    }
    //从气泡或输入框复制出来的富文本可能带内置表情：按片段还原，
    //否则纯文本里只剩一个对象替换符，粘进来是个空白
    if (mime->hasHtml() && mime->html().contains(QStringLiteral("qrc:/res/emoji/"))) {
        cursor.insertFragment(QTextDocumentFragment::fromHtml(mime->html()));
        position = cursor.position();
        normalizeEmoji(document); //粘贴过来的相邻同款表情同样要拆开
        return position;
    }
    if (mime->hasImage())
        return insertImageData(document, position, qvariant_cast<QImage>(mime->imageData()));
    cursor.insertText(mime->text());
    return cursor.position();
}

QVariantList ComposerBridge::parts(QQuickTextDocument *document) const
{
    QVariantList result;
    if (!document || !document->textDocument())
        return result;
    QString text;
    const auto flush = [&] {
        if (text.isEmpty())
            return;
        //@ 只在正文里还留着"@名字"时才算数：用户删掉名字后不该继续提醒对方
        QStringList mentions;
        for (auto it = _mentions.cbegin(); it != _mentions.cend(); ++it) {
            if (text.contains(QLatin1Char('@') + it.key()))
                mentions.append(QString::number(it.value()));
        }
        result.append(QVariantMap{{"kind", "text"}, {"text", text}, {"mentions", mentions}});
        text.clear();
    };
    for (QTextBlock block = document->textDocument()->begin(); block.isValid(); block = block.next()) {
        if (block != document->textDocument()->begin())
            text += QLatin1Char('\n');
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid())
                continue;
            if (fragment.charFormat().isImageFormat()) {
                const auto format = fragment.charFormat().toImageFormat();
                const QString source = format.name();
                //内置表情：把图片还原成 token 并留在同一段文本里，不要拆成图片附件
                const QString emoji = format.property(QTextFormat::ImageAltText).toString();
                if (source.startsWith(QStringLiteral("qrc:/res/emoji/")) && !emoji.isEmpty()) {
                    for (int i = 0; i < fragment.length(); ++i)
                        text += emoji;
                    continue;
                }
                flush();
                const auto inserted = _images.constFind(imageKey(source));
                const QSize pixel = inserted != _images.constEnd()
                    ? inserted->pixelSize : QSize(qRound(format.width()), qRound(format.height()));
                //一个 fragment 可能包含多个相同图片对象，每个都要带上自己的上传文件
                for (int i = 0; i < fragment.length(); ++i) {
                    result.append(QVariantMap{
                        {"kind", "image"},
                        {"source", source},
                        {"upload", inserted != _images.constEnd() ? inserted->uploadPath : QString()},
                        {"width", pixel.width()},
                        {"height", pixel.height()},
                    });
                }
            } else {
                text += fragment.text();
            }
        }
    }
    flush();
    //附件排在文字/图片之后，按添加顺序逐个发送
    for (const QVariant &attachment : _attachments)
        result.append(attachment);
    return result;
}

//选择/拖入视频或文件后追加一条附件记录；同一路径只保留一次
bool ComposerBridge::addAttachment(const QUrl &url, const QString &kind)
{
    if (kind != "video" && kind != "file") {
        setError(tr("不支持的附件类型"));
        return false;
    }
    if (!url.isLocalFile()) {
        setError(tr("请选择本地文件"));
        return false;
    }
    const QFileInfo info(url.toLocalFile());
    if (!info.isFile() || info.size() <= 0) {
        setError(tr("文件不存在或为空"));
        return false;
    }

    const QString path = info.absoluteFilePath();
    for (const QVariant &item : _attachments) {
        if (item.toMap().value("path").toString() == path)
            return true; //重复拖同一个文件不叠加卡片
    }
    if (_attachments.size() >= 20) {
        setError(tr("一次最多添加 20 个附件"));
        return false;
    }
    _attachments.append(QVariantMap{
        {"kind", kind},
        {"path", path},
        {"name", info.fileName()},
        {"size", info.size()},
    });
    setError({});
    emit attachmentsChanged();
    return true;
}

void ComposerBridge::removeAttachment(int index)
{
    if (index < 0 || index >= _attachments.size())
        return;
    _attachments.removeAt(index);
    if (_draftKey != 0) {
        if (_attachments.isEmpty())
            _attachment_drafts.remove(_draftKey);
        else
            _attachment_drafts.insert(_draftKey, _attachments);
    }
    emit attachmentsChanged();
}

void ComposerBridge::clearAttachments()
{
    //发送成功或用户主动清空时，当前会话的暂存也一起删掉
    if (_draftKey != 0)
        _attachment_drafts.remove(_draftKey);
    if (_attachments.isEmpty())
        return;
    _attachments.clear();
    emit attachmentsChanged();
}

void ComposerBridge::switchConversation(QQuickTextDocument *document, qint64 threadId, int peerUid)
{
    const qint64 pendingKey = -qint64(peerUid);
    const qint64 key = threadId > 0 ? threadId : pendingKey;
    if (!document || !document->textDocument() || key == _draftKey)
        return;
    //正在输入时收到建会话回包：保留原文档、光标和附件，只换落库的键。
    if (threadId > 0 && peerUid > 0 && _draftKey == pendingKey) {
        _draftKey = threadId;
        _drafts.remove(pendingKey);
        _attachment_drafts.remove(pendingKey);
        saveDraft();
        return;
    }
    auto *doc = document->textDocument();
    if (_draftKey != 0) {
        const QString raw = doc->toPlainText();
        const bool empty = raw.trimmed().isEmpty()
            && !raw.contains(QChar::ObjectReplacementCharacter);
        _drafts.insert(_draftKey, empty ? QString() : doc->toHtml());
        saveDraft(); //防抖定时器可能还在等，切走前先立即落库
        _draftTimer.stop(); //旧会话的定时器不能带到新会话上
        //附件跟着会话走：用户选好文件临时去看另一条消息，回来时不能空
        if (_attachments.isEmpty())
            _attachment_drafts.remove(_draftKey);
        else
            _attachment_drafts.insert(_draftKey, _attachments);
    }
    //回包也可能在看其他会话时到达；重新打开好友时带回此前暂存的草稿。
    if (threadId > 0 && peerUid > 0 && _drafts.contains(pendingKey)) {
        const QString draft = _drafts.take(pendingKey);
        _drafts.insert(key, draft);
        UserMgr::GetInstance()->SetThreadDraft(threadId, draft);
        _attachment_drafts.insert(key, _attachment_drafts.take(pendingKey));
    }
    _draftKey = key;
    _mentions.clear(); //草稿换人，上一条消息的 @ 名单不能带过来
    {
        //setHtml 也会触发 contentsChanged，别把刚读出来的内容又写回去
        const QSignalBlocker blocker(doc);
        const QString fallback = key > 0 && !_drafts.contains(key)
            ? UserMgr::GetInstance()->ThreadDraft(key) : QString();
        doc->setHtml(_drafts.value(key, fallback));
    }
    if (_document != doc) {
        if (_document)
            disconnect(_document, &QTextDocument::contentsChanged, this, nullptr);
        _document = doc;
        connect(doc, &QTextDocument::contentsChanged, this, [this] {
            _draftTimer.start();
            refreshInlineContent();
        });
    }
    normalizeEmoji(document); //草稿里的连续同款表情要重新拆开
    refreshInlineContent();
    //把这个会话上次留下的附件换回来；没有就是空列表
    _attachments = _attachment_drafts.value(key);
    emit attachmentsChanged();
    setError({});
}

//在光标处插入 "@名字 "：只改文档，光标位置由 QML 按插入长度前移。
//名字里不能带换行，否则会把一条消息拆成多行
// 内置表情插成图片：输入框所见即所得，发送时 parts() 再还原成 token 文本
int ComposerBridge::insertEmoji(QQuickTextDocument *document, int position,
                                const QString &token, const QUrl &url,
                                int width, int height)
{
    if (!document || !document->textDocument() || token.isEmpty() || width <= 0 || height <= 0)
        return position;

    QTextDocument *doc = document->textDocument();
    QTextCursor cursor(doc);
    cursor.setPosition(qBound(0, position, qMax(0, doc->characterCount() - 1)));

    QTextImageFormat format;
    format.setName(url.toString());
    //按素材比例给宽高：富文本图片会拉伸到给定矩形，正方形会把 122×100 压扁
    format.setWidth(width);
    format.setHeight(height);
    // alt 既给无障碍用，也是取文本时识别"这是表情而不是图片消息"的依据
    format.setProperty(QTextFormat::ImageAltText, token);
    //每个表情一个唯一格式号，强制相邻同款表情留在各自的 fragment 里
    format.setProperty(EmojiSerialProperty, ++_emojiSerial);
    //图片顶部贴行顶；高度不超过字体行高，整行的行高与纯文字行完全一致
    format.setVerticalAlignment(QTextCharFormat::AlignTop);
    cursor.insertImage(format);
    refreshInlineContent();
    return cursor.position();
}

//草稿是带格式的 HTML，唯一格式号不在 HTML 里；恢复后重新编号，
//否则连打两个同款表情又会并成一个 fragment，只显示第一个
void ComposerBridge::normalizeEmoji(QQuickTextDocument *document)
{
    if (!document || !document->textDocument())
        return;
    QTextDocument *doc = document->textDocument();
    QTextCursor edit(doc);
    edit.beginEditBlock();
    const int count = doc->characterCount();
    for (int pos = 0; pos < count; ++pos) {
        QTextCursor probe(doc);
        probe.setPosition(pos);
        const QTextCharFormat format = probe.charFormat();
        if (!format.isImageFormat())
            continue;
        QTextImageFormat image = format.toImageFormat();
        if (!image.name().startsWith(QStringLiteral("qrc:/res/emoji/"))
            || image.property(QTextFormat::ImageAltText).toString().isEmpty()) {
            continue;
        }
        QTextCursor target(doc);
        target.setPosition(pos);
        //文档末尾的表情后面没有字符，选区终点要夹在有效范围内
        target.setPosition(qMin(pos + 1, doc->characterCount() - 1),
                           QTextCursor::KeepAnchor);
        image.setProperty(EmojiSerialProperty, ++_emojiSerial);
        target.setCharFormat(image);
    }
    edit.endEditBlock();
}

bool ComposerBridge::insertMention(QQuickTextDocument *document, int position,
                                   const QString &name, int uid)
{
    if (!document || !document->textDocument() || uid <= 0 || name.isEmpty())
        return false;
    QString clean = name;
    clean.replace(QLatin1Char('\n'), QLatin1Char(' '));
    clean.replace(QLatin1Char('\r'), QLatin1Char(' '));
    QTextCursor cursor(document->textDocument());
    cursor.setPosition(qMax(0, position));
    cursor.insertText(QStringLiteral("@%1 ").arg(clean));
    _mentions.insert(clean, uid);
    return true;
}

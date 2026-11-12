#include "composerbridge.h"

#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QMimeData>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QUuid>

#include "usermgr.h"

namespace {
constexpr int UploadMaxSide = 1600; //上传压缩图的长边上限，点开大图时不至于模糊
constexpr int ThumbMaxWidth = 480; //气泡缩略图按 2x 屏幕准备，实际显示再缩到 220 宽
constexpr int ThumbMaxHeight = 360;
constexpr int PreviewMaxWidth = 160; //输入区里的显示尺寸，与图片像素尺寸分开
constexpr int PreviewMaxHeight = 100;
constexpr int ImageQuality = 86;

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
    setError({});
    return cursor.position();
}

int ComposerBridge::paste(QQuickTextDocument *document, int position, int selectionEnd)
{
    if (!document || !document->textDocument())
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
        result.append(QVariantMap{{"kind", "text"}, {"text", text}});
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
                flush();
                const auto format = fragment.charFormat().toImageFormat();
                const QString source = format.name();
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
    return result;
}

void ComposerBridge::switchConversation(QQuickTextDocument *document, int uid)
{
    if (!document || !document->textDocument() || uid == _uid)
        return;
    auto *doc = document->textDocument();
    if (_uid != 0)
        _drafts.insert(_uid, doc->toHtml());
    _uid = uid;
    doc->setHtml(_drafts.value(uid));
    setError({});
}

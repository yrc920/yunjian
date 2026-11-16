#include "messagetext.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextObjectInterface>
#include <QVariantMap>

#include <algorithm>

namespace {

// 自定义对象类型与属性键：宽度用于占位，URL 供覆盖层取图
enum {
    EmojiObjectType = QTextFormat::UserObject + 1,
    EmojiUrlProperty = QTextFormat::UserProperty + 1,
    EmojiSizeProperty = QTextFormat::UserProperty + 2,
};

// 文字的真实行高：QFontMetrics::height() 不含行距，和 QTextLayout 排出来的行盒
// 能差 3~4px，直接拿它固定行高会把文字压扁、表情也跟着偏
qreal naturalLineHeight(const QFont &font)
{
    QTextLayout layout(QStringLiteral("国"), font);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid())
        line.setLineWidth(10000);
    layout.endLayout();
    return line.isValid() ? line.height() : QFontMetricsF(font).height();
}

// 行内表情占位：宽度按表情显示尺寸预留，高度只占 1px，
// 完全不参与行高；图片由气泡的覆盖层按这个位置绘制
class EmojiObjectInterface : public QObject, public QTextObjectInterface
{
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &format) override
    {
        return QSizeF(format.property(EmojiSizeProperty).toDouble(), 1.0);
    }

    void drawObject(QPainter *, const QRectF &, QTextDocument *, int, const QTextFormat &) override
    {
        //占位本身不画任何东西
    }
};

EmojiObjectInterface *emojiInterface()
{
    static EmojiObjectInterface instance;
    return &instance;
}

} // namespace

MessageText::MessageText(QObject *parent)
    : QObject(parent)
{
}

QVariantList MessageText::apply(QQuickTextDocument *document, const QString &text,
                                const QColor &color, int pixelSize, int emojiSize,
                                const QVariantList &tokens)
{
    QVariantList spots;
    if (!document || !document->textDocument() || pixelSize <= 0)
        return spots;

    QTextDocument *doc = document->textDocument();
    doc->clear();

    QFont font = doc->defaultFont();
    font.setPixelSize(pixelSize);
    doc->setDefaultFont(font);
    //TextEdit 默认给文档留 4px 边距，气泡按固定内边距排版，这里必须清零
    doc->setDocumentMargin(0);
    doc->documentLayout()->registerHandler(EmojiObjectType, emojiInterface());

    // token 长的优先匹配，避免短 token 抢掉长 token 的前缀
    QHash<QString, QString> tokenUrl;
    QStringList ordered;
    for (const QVariant &item : tokens) {
        const QVariantMap map = item.toMap();
        const QString token = map.value(QStringLiteral("token")).toString();
        const QString file = map.value(QStringLiteral("file")).toString();
        if (token.isEmpty() || file.isEmpty())
            continue;
        tokenUrl.insert(token, QStringLiteral("qrc:/res/emoji/%1.png").arg(file));
        ordered.append(token);
    }
    std::sort(ordered.begin(), ordered.end(), [](const QString &a, const QString &b) {
        return a.size() > b.size();
    });

    QTextCharFormat textFormat;
    textFormat.setForeground(color);

    QTextCursor cursor(doc);
    cursor.beginEditBlock();

    QString plain;
    const auto flush = [&] {
        if (plain.isEmpty())
            return;
        cursor.insertText(plain, textFormat);
        plain.clear();
    };

    int index = 0;
    while (index < text.size()) {
        QString matched;
        for (const QString &token : ordered) {
            if (token.size() > matched.size()
                && text.mid(index, token.size()) == token) {
                matched = token;
                break;
            }
        }
        if (matched.isEmpty()) {
            plain += text.at(index);
            index += 1;
            continue;
        }

        flush();
        QTextCharFormat format;
        format.setObjectType(EmojiObjectType);
        format.setProperty(EmojiUrlProperty, tokenUrl.value(matched));
        format.setProperty(EmojiSizeProperty, emojiSize);
        const int position = cursor.position();
        cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
        spots.append(QVariantMap{
            {QStringLiteral("pos"), position},
            {QStringLiteral("url"), tokenUrl.value(matched)},
        });
        index += matched.size();
    }
    flush();

    cursor.endEditBlock();

    // 只有表情的行没有文字，行高会退化成占位高度（1px）；给每个块设成字体真实行高，
    // 纯文字、纯表情、混排的行高才完全一致，表情也能按行盒中心对齐
    const qreal lineHeight = naturalLineHeight(font);
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        QTextCursor blockCursor(block);
        QTextBlockFormat format = block.blockFormat();
        format.setLineHeight(lineHeight, QTextBlockFormat::FixedHeight);
        blockCursor.setBlockFormat(format);
    }

    return spots;
}

QVariantMap MessageText::lineMetrics(QQuickTextDocument *document, int position) const
{
    QVariantMap result{{QStringLiteral("top"), 0.0}, {QStringLiteral("height"), 0.0}};
    if (!document || !document->textDocument() || position < 0)
        return result;

    QTextDocument *doc = document->textDocument();
    const QTextBlock block = doc->findBlock(position);
    if (!block.isValid())
        return result;

    const QTextLayout *layout = block.layout();
    const QTextLine line = layout->lineForTextPosition(position - block.position());
    if (!line.isValid())
        return result;

    // 块内行高由 apply() 统一设成固定值；只有没设过的块才退回行自身高度
    const QTextBlockFormat format = block.blockFormat();
    qreal height = format.lineHeight();
    if (format.lineHeightType() != QTextBlockFormat::FixedHeight || height <= 0)
        height = line.height();

    // 行盒顶部 = 块顶部 + 块内行号 × 行高。
    // 不用 line.y()：它会带上字体的 leading 偏移，纯对象行还会贴到行底
    const qreal blockTop = doc->documentLayout()->blockBoundingRect(block).top();
    result[QStringLiteral("top")] = blockTop + line.lineNumber() * height;
    result[QStringLiteral("height")] = height;
    return result;
}

#include "messagetext.moc"

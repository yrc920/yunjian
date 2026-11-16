#ifndef MESSAGETEXT_H
#define MESSAGETEXT_H

#include <QObject>
#include <QQuickTextDocument>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// 消息正文排版：内置表情在文档里只占一个等宽占位对象，不参与行高计算。
//
// 为什么不用 <img>：富文本图片按自身高度抬高行盒，24px 会把 14px 正文的行高
// 从 20px 撑到 24px 以上；改成"对象 + 自绘"又会被对象矩形裁剪，底部画不全。
// 所以这里只留占位（宽 = 表情尺寸，高 = 1px），真正的图片由气泡用覆盖层绘制：
// 行高保持字体的自然行高，图片可以画得比行高更大，按行盒居中且不被裁剪。
class MessageText : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit MessageText(QObject *parent = nullptr);

    // tokens: [{ token, file }]，与 Emoji.qml 的目录一致。
    // 返回每个表情在文档里的位置 [{pos, url}]：正文只留等宽占位，
    // 真正的图片由气泡用覆盖层画，才能画得比行高更大而不被裁剪
    Q_INVOKABLE QVariantList apply(QQuickTextDocument *document, const QString &text,
                                   const QColor &color, int pixelSize, int emojiSize,
                                   const QVariantList &tokens);

    // 某个字符所在行的行盒：{top, height}，坐标与 TextEdit 内容一致。
    // 不能用 positionToRectangle 的 y/height：纯表情行没有文字，行盒高度会退化成
    // 占位对象的 1px，光标矩形贴在行底，按它居中必然偏下
    Q_INVOKABLE QVariantMap lineMetrics(QQuickTextDocument *document, int position) const;
};

#endif // MESSAGETEXT_H

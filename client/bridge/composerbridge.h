#ifndef COMPOSERBRIDGE_H
#define COMPOSERBRIDGE_H

#include <QObject>
#include <QHash>
#include <QImage>
#include <QQuickTextDocument>
#include <QSize>
#include <QUrl>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

// 富文本输入边界：插入本地图片、粘贴纯文本、按文档顺序读取内容。
// 图片分成两份：输入区与气泡用缩略图显示，真正上传的是长边 1600 的压缩图。
class ComposerBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit ComposerBridge(QObject *parent = nullptr);
    QString errorText() const { return _errorText; }
    Q_INVOKABLE int insertImage(QQuickTextDocument *document, int position, const QUrl &url);
    Q_INVOKABLE int paste(QQuickTextDocument *document, int position, int selectionEnd);
    Q_INVOKABLE void switchConversation(QQuickTextDocument *document, int uid);
    Q_INVOKABLE QVariantList parts(QQuickTextDocument *document) const;

signals:
    void errorTextChanged();

private:
    //一次插入的图片：上传文件在本机临时目录，尺寸是气泡缩略图的像素大小
    struct InsertedImage {
        QString uploadPath;
        QSize pixelSize;
    };

    int insertImageData(QQuickTextDocument *document, int position, const QImage &image);
    void setError(const QString &message);
    QString savePendingImage(const QByteArray &data, const QByteArray &suffix);
    static QString imageKey(const QString &source); //缩略图 data URL 的短哈希
    QString _errorText;
    int _uid = 0;
    QHash<int, QString> _drafts;
    QHash<QString, InsertedImage> _images; //缩略图哈希 -> 上传文件与尺寸
};

#endif // COMPOSERBRIDGE_H

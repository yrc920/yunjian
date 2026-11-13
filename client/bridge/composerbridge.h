#ifndef COMPOSERBRIDGE_H
#define COMPOSERBRIDGE_H

#include <QObject>
#include <QHash>
#include <QImage>
#include <QQuickTextDocument>
#include <QSize>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

// 富文本输入边界：插入本地图片、粘贴纯文本、按文档顺序读取内容。
// 图片分成两份：输入区与气泡用缩略图显示，真正上传的是长边 1600 的压缩图。
class ComposerBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    //本次待发送的视频/文件：可以一次添加多个，输入区按卡片逐条列出
    Q_PROPERTY(QVariantList attachments READ attachments NOTIFY attachmentsChanged)
    Q_PROPERTY(bool hasAttachment READ hasAttachment NOTIFY attachmentsChanged)

public:
    explicit ComposerBridge(QObject *parent = nullptr);
    QString errorText() const { return _errorText; }
    QVariantList attachments() const { return _attachments; }
    bool hasAttachment() const { return !_attachments.isEmpty(); }
    Q_INVOKABLE int insertImage(QQuickTextDocument *document, int position, const QUrl &url);
    Q_INVOKABLE int paste(QQuickTextDocument *document, int position, int selectionEnd);
    Q_INVOKABLE void switchConversation(QQuickTextDocument *document, int uid);
    Q_INVOKABLE QVariantList parts(QQuickTextDocument *document) const;
    //按扩展名区分拖入/粘贴的本地文件：image 走富文本，video/file 走附件列表
    Q_INVOKABLE QString fileKind(const QUrl &url) const;
    //剪贴板里的本地文件（Finder/资源管理器复制的文件），没有时返回空，交给 paste 处理文本和位图
    Q_INVOKABLE QVariantList clipboardFiles() const;
    //kind 只接受 video/file；同一路径不会重复添加
    Q_INVOKABLE bool addAttachment(const QUrl &url, const QString &kind);
    Q_INVOKABLE void removeAttachment(int index);
    Q_INVOKABLE void clearAttachments();

signals:
    void errorTextChanged();
    void attachmentsChanged();

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
    QVariantList _attachments; //待发送的视频/文件，每项含 kind/path/name/size
};

#endif // COMPOSERBRIDGE_H

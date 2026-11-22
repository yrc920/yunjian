#ifndef COMPOSERBRIDGE_H
#define COMPOSERBRIDGE_H

#include <QObject>
#include <QHash>
#include <QImage>
#include <QQuickTextDocument>
#include <QSize>
#include <QTimer>
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
    //草稿按会话保存：thread_id 是单聊与群聊共用的键，切换会话不丢未发送内容
    Q_INVOKABLE void switchConversation(QQuickTextDocument *document, qint64 threadId, int peerUid);
    //在光标处插入 @名字，并记下名字对应的 uid；发送时按纯文本里是否还含有这个名字过滤
    Q_INVOKABLE bool insertMention(QQuickTextDocument *document, int position,
                                   const QString &name, int uid);
    //在光标处插入一个内置表情：编辑器里显示图片，取文本时还原成 token。
    //size 是显示宽高（按素材比例，不是正方形），避免富文本拉伸变形
    Q_INVOKABLE int insertEmoji(QQuickTextDocument *document, int position,
                                const QString &token, const QUrl &url, int width, int height);
    Q_INVOKABLE QVariantList parts(QQuickTextDocument *document) const;
    //发送成功后立即清掉当前会话草稿：不等 600ms 防抖，列表不会短暂显示"未寄出"
    Q_INVOKABLE void clearDraft();
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
    //草稿从 HTML 恢复后，给每个表情重新发一个唯一格式号，避免相邻同款表情被合并
    void normalizeEmoji(QQuickTextDocument *document);
    //把输入框当前内容写进本地库：停止输入后由定时器触发，切会话时立即触发
    void saveDraft();
    QString _errorText;
    qint64 _draftKey = 0; //已有会话用 thread_id，新好友用负 uid 临时区分，只在内存暂存
    int _emojiSerial = 0; //表情格式序号：每个表情一个，仅用于拆开相邻 fragment
    QHash<qint64, QString> _drafts;
    //附件按会话暂存：切走再回来时待发送的文件还在，不因为换会话被动丢掉
    QHash<qint64, QVariantList> _attachment_drafts;
    QTextDocument *_document = nullptr; //当前绑定的输入文档，切会话复用同一个对象
    QTimer _draftTimer;                 //停止输入 600ms 后落库，避免每敲一个字写一次盘
    QHash<QString, int> _mentions; //@显示名 -> uid，发送时按正文里是否还有该名字过滤
    QHash<QString, InsertedImage> _images; //缩略图哈希 -> 上传文件与尺寸
    QVariantList _attachments; //待发送的视频/文件，每项含 kind/path/name/size
};

#endif // COMPOSERBRIDGE_H

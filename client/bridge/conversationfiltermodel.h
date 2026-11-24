#ifndef CONVERSATIONFILTERMODEL_H
#define CONVERSATIONFILTERMODEL_H

#include <QSortFilterProxyModel>
#include <QtQml/qqmlregistration.h>

class ConversationModel;

/**
 * @brief 会话列表的本地筛选：只筛会话名与最后一条消息，不触发服务端搜索。
 *
 * 聊天窗口绑这个代理；转发弹层仍读完整的 ConversationModel，
 * 因此筛选不会让“可转发的会话”变少。
 */
class ConversationFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)

public:
    explicit ConversationFilterModel(QObject *parent = nullptr);

    void setSource(ConversationModel *source);

    int count() const;
    QString filterText() const;
    void setFilterText(const QString &text);


signals:
    void countChanged();
    void filterTextChanged();

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;

private:
    QString _filter;
    int _nameRole = 0;
    int _textRole = 0;
};

#endif // CONVERSATIONFILTERMODEL_H

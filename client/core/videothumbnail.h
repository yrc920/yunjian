#ifndef VIDEOTHUMBNAIL_H
#define VIDEOTHUMBNAIL_H

#include <QImage>
#include <QMediaPlayer>
#include <QObject>
#include <QTimer>
#include <QVideoSink>

/******************************************************************************
 *
 * @file       videothumbnail.h
 * @brief      取视频第一帧作为聊天预览
 *
 * QMediaPlayer 异步加载本地视频，QVideoSink 收到第一帧后转换成 QImage 返回；
 * 解码失败、没有视频轨或超时都发 failed，由调用方退回通用视频卡片。
 *
 *****************************************************************************/

class VideoThumbnailer : public QObject
{
    Q_OBJECT

public:
    explicit VideoThumbnailer(QObject *parent = nullptr);

    void grab(const QString &path); //开始取首帧，结果通过信号返回

signals:
    void finished(const QImage &image);
    void failed();

private:
    void finishWith(const QImage &image);
    void fail();

    QMediaPlayer _player;
    QVideoSink _sink;
    QTimer _timeout;
    bool _active = false;
};

#endif // VIDEOTHUMBNAIL_H

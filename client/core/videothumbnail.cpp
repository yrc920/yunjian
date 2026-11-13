#include "videothumbnail.h"

#include <QUrl>
#include <QVideoFrame>

namespace {
constexpr int GrabTimeoutMs = 6000; //本地视频解码首帧通常几百毫秒，超过这个时间按失败处理
}

VideoThumbnailer::VideoThumbnailer(QObject *parent) : QObject(parent)
{
    _player.setVideoSink(&_sink);
    QObject::connect(&_sink, &QVideoSink::videoFrameChanged, this,
        [this](const QVideoFrame &frame) {
        if (!_active || !frame.isValid())
            return;
        const QImage image = frame.toImage();
        if (image.isNull())
            return;
        finishWith(image); //第一帧够用，取到就停止解码
    });
    QObject::connect(&_player, &QMediaPlayer::errorOccurred, this,
        [this](QMediaPlayer::Error, const QString &) {
        if (_active)
            fail();
    });

    _timeout.setSingleShot(true);
    _timeout.setInterval(GrabTimeoutMs);
    QObject::connect(&_timeout, &QTimer::timeout, this, [this] {
        if (_active)
            fail();
    });
}

void VideoThumbnailer::grab(const QString &path)
{
    if (path.isEmpty())
        return;
    _active = true;
    _player.setSource(QUrl::fromLocalFile(path));
    _player.play(); //只取首帧，拿到 frame 后立即 stop
    _timeout.start();
}

void VideoThumbnailer::finishWith(const QImage &image)
{
    _active = false;
    _timeout.stop();
    _player.stop();
    _player.setSource(QUrl()); //释放解码器与文件句柄
    emit finished(image);
}

void VideoThumbnailer::fail()
{
    _active = false;
    _timeout.stop();
    _player.stop();
    _player.setSource(QUrl());
    emit failed();
}

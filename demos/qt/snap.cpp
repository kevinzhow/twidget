// Parity snapshots of the Qt reference: each card alone, at 360 CSS px, painted at fixed renderer times
// with the core's clock held (Renderer::set_test_clock), into PNGs the Flutter port's parity mode is
// compared against (flutter_twidget/tool/parity.py).
//   twidget-qt-snap <out dir> <times, seconds, comma separated> <corpus-relative card, or an absolute path> ...
// Per card: <out>/<card with / as __>@<time>.png for each time, @<first time>b.png the first time again
// 1.1 s later (a card whose two first frames differ changes on its own, from script timers), and
// <card>.json with the height, paints and the host's error, <card>.list.json the last display list. The clock is held at 1000 s while the card
// loads and settles (2 s), then at 1000 + time for each snapshot.
#include "widget_view.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QThread>

#include <cstdio>

namespace {

constexpr double kBase = 1000;

void pump(int ms) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
}

QImage shot(WidgetView& view) {
    double dpr = view.devicePixelRatioF();
    QImage img(QSize(int(std::lround(view.width() * dpr)), int(std::lround(view.height() * dpr))), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    view.render(&img, QPoint(), QRegion(), QWidget::DrawChildren);
    return img;
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc < 4) {
        std::fprintf(stderr, "usage: twidget-qt-snap <out dir> <times> <card> ...\n");
        return 2;
    }
    QString out = QString::fromLocal8Bit(argv[1]);
    QDir().mkpath(out);
    QList<double> times;
    for (const QString& t : QString::fromLocal8Bit(argv[2]).split(',')) times.push_back(t.toDouble());
    auto resources = twidget::SandboxResources::fromDirectory(TW_WIDGET_JS);

    for (int i = 3; i < argc; i++) {
        QString card = QString::fromLocal8Bit(argv[i]);
        QString slug = QString(card.startsWith('/') ? QFileInfo(card).fileName() : card).replace('/', "__");
        if (QFile::exists(out + "/" + slug + ".json")) continue;  // done by a run that died later
        QFile f(card.startsWith('/') ? card : QString(TW_CORPUS) + "/" + card);
        if (!f.open(QIODevice::ReadOnly)) continue;
        std::string html = f.readAll().toStdString();
        twlite::Renderer::set_test_clock(kBase);
        // The core's display list, as painted last (qt_renderer.cpp), next to the PNGs.
        qputenv("TWIDGET_QT_DUMP_LIST", (out + "/" + slug + ".list.json").toLocal8Bit());
        int paints = 0;
        QString error;
        double height = 0;
        {
            WidgetView view(html, false, 360, resources);
            QString status;
            view.onStatus = [&status](const QString& s) { status = s; };
            view.show();
            view.start();
            pump(2000);
            auto save = [&](const QString& name) { shot(view).save(out + "/" + slug + "@" + name + ".png"); };
            for (int k = 0; k < times.size(); k++) {
                twlite::Renderer::set_test_clock(kBase + times[k]);
                pump(300);
                save(QString::number(times[k]));
                if (k == 0) {
                    pump(1100);
                    save(QString::number(times[k]) + "b");
                }
            }
            height = view.height();
            error = status.contains("error") || status.startsWith("dead") ? status : QString();
            paints = status.section("paints ", 1).section(' ', 0, 0).toInt();
            view.hide();
        }
        QFile meta(out + "/" + slug + ".json");
        if (meta.open(QIODevice::WriteOnly)) {
            QString esc = QString(error).replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n").replace('\r', "\\r").replace('\t', "\\t");
            meta.write(QStringLiteral("{\"card\":\"%1\",\"height\":%2,\"paints\":%3,\"error\":\"%4\"}\n").arg(card).arg(height).arg(paints).arg(esc).toUtf8());
        }
        std::printf("TW_SNAP %s\n", card.toUtf8().constData());
        std::fflush(stdout);
    }
    return 0;
}

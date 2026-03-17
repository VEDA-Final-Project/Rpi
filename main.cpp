#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QUdpSocket>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QUrl>
#include <QDebug>
#include <QDateTime>
#include <QSocketNotifier>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <QTimer>
struct CameraInfo {
    QString ip;
    QString user;
    QString pass;
};

class VmsController : public QWidget {
    Q_OBJECT

public:
    VmsController(QWidget *parent = nullptr) : QWidget(parent) {
        // 1. 카메라 정보 로드
        cams[1] = {"192.168.0.23", "admin", "1team@@@"};
        cams[2] = {"192.168.0.34", "admin", "1team@@@"};
        cams[3] = {"192.168.0.76", "admin", "1team@@@"};
        cams[4] = {"192.168.0.78", "admin", "1team@@@"};

        setupDesign();
        setupUI();
        setupPlayer();
        setupJoystick();

        udpSocket = new QUdpSocket(this);
        dbTabs << "주차 이력" << "사용자" << "장치 로그" << "차량 정보" << "주차구역 현황";

        // 초기 실행 시 채널 1번 자동 재생
        QTimer::singleShot(500, [this](){ changeChannel(1); });
    }

private:
    QUdpSocket *udpSocket;
    QMediaPlayer *player;
    QVideoWidget *videoWidget;
    QMap<int, CameraInfo> cams;
    QMap<int, QPushButton*> channelButtons;
    
    QStringList dbTabs;
    int currentDbIdx = 0;
    int joyFd = -1;
    QSocketNotifier *joyNotifier = nullptr;
    
    QPushButton *dbSwitchBtn;
    const QString desktopIp = "192.168.0.100"; 
    const quint16 port = 12345;

    void setupDesign() {
        this->resize(800, 480);
        this->setStyleSheet(
            "QWidget { background-color: #1a1d23; color: #e1e1e1; font-family: 'Segoe UI', Arial; }"
            "QPushButton { background-color: #2d323e; border: 1px solid #3f4552; border-radius: 4px; padding: 10px; font-weight: bold; }"
            "QPushButton:pressed { background-color: #00ff88; color: #1a1d23; }"
            "QPushButton#channelBtn { border-left: 4px solid #00ff88; text-align: left; padding-left: 15px; }"
            "QPushButton#channelBtn:checked { background-color: #3f4552; border-left: 4px solid #ffffff; }"
            "QPushButton#dbBtn { background-color: #252a33; border: 2px solid #00ff88; font-size: 18px; }"
            "QLabel#title { color: #00ff88; font-size: 20px; font-weight: bold; }"
        );
    }

    void setupUI() {
        QHBoxLayout *midLayout = new QHBoxLayout(this);

        // --- 왼쪽 사이드바 ---
        QVBoxLayout *sideBar = new QVBoxLayout();
        QLabel *titleLabel = new QLabel("VEDA 1Team VMS");
        titleLabel->setObjectName("title");
        sideBar->addWidget(titleLabel);

        for(int i=1; i<=4; ++i) {
            QPushButton *chBtn = new QPushButton(QString("CH %1").arg(i));
            chBtn->setObjectName("channelBtn");
            chBtn->setCheckable(true);
            chBtn->setFixedSize(140, 45);
            connect(chBtn, &QPushButton::clicked, [this, i](){ changeChannel(i); });
            sideBar->addWidget(chBtn);
            channelButtons[i] = chBtn;
        }

        sideBar->addSpacing(20);
        sideBar->addWidget(new QLabel("----- MEDIA -----"));
        QPushButton *capBtn = new QPushButton("CAPTURE");
        QPushButton *recBtn = new QPushButton("RECORD");
        sideBar->addWidget(capBtn);
        sideBar->addWidget(recBtn);
        sideBar->addStretch();
        midLayout->addLayout(sideBar);

        // --- 우측 메인 영역 ---
        QVBoxLayout *mainArea = new QVBoxLayout();
        videoWidget = new QVideoWidget(this);
        videoWidget->setStyleSheet("background-color: #000; border: 1px solid #333;");
        mainArea->addWidget(videoWidget, 1);

        dbSwitchBtn = new QPushButton("DB TAB: [ 주차 이력 ]");
        dbSwitchBtn->setObjectName("dbBtn");
        dbSwitchBtn->setFixedHeight(70);
        connect(dbSwitchBtn, &QPushButton::clicked, this, &VmsController::handleDbSwitch);
        mainArea->addWidget(dbSwitchBtn);

        midLayout->addLayout(mainArea, 1);

        connect(capBtn, &QPushButton::clicked, [this](){ sendCmd("CMD:CAP:NOW"); });
        connect(recBtn, &QPushButton::clicked, [this](){ sendCmd("CMD:REC:TOGGLE"); });
    }

    void setupPlayer() {
        player = new QMediaPlayer(this, QMediaPlayer::LowLatency);
        player->setVideoOutput(videoWidget);
    }

    void changeChannel(int ch) {
        qDebug() << "[ACTION] Channel" << ch << "Selected";
        for(int i=1; i<=4; ++i) channelButtons[i]->setChecked(i == ch);

        player->stop();
        
        QUrl url(QString("rtsp://%1:554/profile9/media.smp").arg(cams[ch].ip));
        url.setUserName(cams[ch].user);
        url.setPassword(cams[ch].pass);

        player->setMedia(url);
        player->play();

        sendCmd(QString("CMD:CH:%1").arg(ch));
    }

    void handleDbSwitch() {
        currentDbIdx = (currentDbIdx + 1) % dbTabs.size();
        dbSwitchBtn->setText(QString("DB TAB: [ %1 ]").arg(dbTabs[currentDbIdx]));
        sendCmd(QString("CMD:DB:TAB:%1").arg(currentDbIdx));
    }

    void setupJoystick() {
        joyFd = open("/dev/input/event4", O_RDONLY | O_NONBLOCK);
        if (joyFd >= 0) {
            joyNotifier = new QSocketNotifier(joyFd, QSocketNotifier::Read, this);
            connect(joyNotifier, &QSocketNotifier::activated, this, &VmsController::readJoystick);
        }
    }

    void readJoystick() {
        struct input_event ev;
        while (read(joyFd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_KEY && ev.value == 1 && ev.code == 288) sendCmd("CMD:CAP:NOW");
            else if (ev.type == EV_ABS) {
                if (ev.code != ABS_X && ev.code != ABS_Y) continue;
                if (ev.value > 200) sendCmd((ev.code == ABS_X) ? "CMD:PTZ:RIGHT" : "CMD:PTZ:DOWN");
                else if (ev.value < 50) sendCmd((ev.code == ABS_X) ? "CMD:PTZ:LEFT" : "CMD:PTZ:UP");
            }
        }
    }

    void sendCmd(QString msg) {
        udpSocket->writeDatagram(msg.toUtf8(), QHostAddress(desktopIp), port);
        qDebug() << "[UDP SENT] " << msg;
    }
};

int main(int argc, char *argv[]) {
    // [보정] 하드웨어 가속 라이브러리 강제 사용
    qputenv("GST_V4L2_USE_LIBV4L2", "1");
    
    QApplication a(argc, argv);
    VmsController w;
    w.showFullScreen();
    return a.exec();
}

#include "main.moc"
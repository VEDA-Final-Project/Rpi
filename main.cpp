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

class VmsController : public QWidget {
    Q_OBJECT

public:
    VmsController(QWidget *parent = nullptr) : QWidget(parent) {
        setupDesign();
        setupUI();
        setupPlayer();
        setupJoystick();

        udpSocket = new QUdpSocket(this);
        dbTabs << "주차 이력" << "사용자" << "장치 로그" << "차량 정보" << "주차구역 현황";
        
        qDebug() << "[SYSTEM] Qt 싱글앱 통합 완료. 디자인 최적화 모드 실행 중.";
    }

private:
    QUdpSocket *udpSocket;
    QMediaPlayer *player;
    QVideoWidget *videoWidget;
    
    QStringList dbTabs;
    int currentDbIdx = 0;
    int joyFd = -1;
    QSocketNotifier *joyNotifier = nullptr;
    
    QPushButton *dbSwitchBtn;
    const QString desktopIp = "192.168.0.100"; // 필요시 192.168.0.96으로 변경
    const quint16 port = 12345;

    void setupDesign() {
        this->resize(800, 480);
        this->setStyleSheet(
            "QWidget { background-color: #1a1d23; color: #e1e1e1; font-family: 'Segoe UI', Arial; }"
            "QPushButton { background-color: #2d323e; border: 1px solid #3f4552; border-radius: 4px; padding: 10px; font-weight: bold; }"
            "QPushButton:pressed { background-color: #3f4552; color: #00ff88; }"
            "QPushButton#channelBtn { border-left: 4px solid #00ff88; text-align: left; padding-left: 15px; }"
            "QPushButton#captureBtn { border: 1px solid #3498db; color: #3498db; }"
            "QPushButton#recordBtn { border: 1px solid #e74c3c; color: #e74c3c; }"
            "QPushButton#dbBtn { background-color: #252a33; border: 2px solid #00ff88; font-size: 18px; color: #ffffff; }"
            "QLabel#title { color: #00ff88; font-size: 20px; font-weight: bold; }"
        );
    }

    void setupUI() {
        QHBoxLayout *midLayout = new QHBoxLayout(this);

        // --- 1. 왼쪽 사이드바 (사용자님이 원하신 그 레이아웃) ---
        QVBoxLayout *sideBar = new QVBoxLayout();
        QLabel *titleLabel = new QLabel("VEDA");
        titleLabel->setObjectName("title");
        sideBar->addWidget(titleLabel);

        for(int i=1; i<=4; ++i) {
            QPushButton *chBtn = new QPushButton(QString("CHANNEL %1").arg(i));
            chBtn->setObjectName("channelBtn");
            chBtn->setFixedSize(140, 45);
            connect(chBtn, &QPushButton::clicked, [this, i](){ sendCmd(QString("CMD:CH:%1").arg(i)); });
            sideBar->addWidget(chBtn);
        }

        sideBar->addSpacing(20);
        sideBar->addWidget(new QLabel("--- MEDIA ---"));

        QPushButton *capBtn = new QPushButton("IMAGE CAPTURE");
        capBtn->setObjectName("captureBtn");
        capBtn->setFixedSize(140, 45);
        
        QPushButton *recBtn = new QPushButton("VIDEO RECORD");
        recBtn->setObjectName("recordBtn");
        recBtn->setFixedSize(140, 45);

        sideBar->addWidget(capBtn);
        sideBar->addWidget(recBtn);
        sideBar->addStretch();
        midLayout->addLayout(sideBar);

        // --- 2. 우측 메인 영역 (통합 영상 위젯 + DB 버튼) ---
        QVBoxLayout *mainArea = new QVBoxLayout();
        
        videoWidget = new QVideoWidget(this);
        videoWidget->setStyleSheet("background-color: #000; border: 1px solid #333;");
        mainArea->addWidget(videoWidget, 1);

        QPushButton *toggleBtn = new QPushButton("LIVE VIEW ON / OFF");
        toggleBtn->setFixedHeight(40);
        connect(toggleBtn, &QPushButton::clicked, this, &VmsController::toggleVideo);
        mainArea->addWidget(toggleBtn);

        dbSwitchBtn = new QPushButton("DB TAB: [ 주차 이력 ]");
        dbSwitchBtn->setObjectName("dbBtn");
        dbSwitchBtn->setFixedHeight(80);
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

    void toggleVideo() {
        if (player->state() == QMediaPlayer::PlayingState) {
            player->stop();
            logAction("Video Stopped");
        } else {
            // [해결] 특수문자(@)를 포함한 인증 정보를 안전하게 설정
            QUrl url("rtsp://192.168.0.23:554/profile9/media.smp");
            url.setUserName("admin");
            url.setPassword("1team@@@"); // QUrl이 자동으로 퍼센트 인코딩을 처리합니다.

            player->setMedia(url);
            player->play();
            logAction("Starting Integrated Video (Profile 9)");
        }
    }

    void handleDbSwitch() {
        currentDbIdx = (currentDbIdx + 1) % dbTabs.size();
        dbSwitchBtn->setText(QString("DB TAB: [ %1 ]").arg(dbTabs[currentDbIdx]));
        logAction(QString("DB Tab Changed: %1").arg(dbTabs[currentDbIdx]));
        sendCmd(QString("CMD:DB:TAB:%1").arg(currentDbIdx));
    }

    // --- 조이스틱 및 통신 (기존 로직 유지) ---
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

    void logAction(QString action) {
        qDebug() << "[" << QDateTime::currentDateTime().toString("HH:mm:ss") << "] " << action;
    }
};

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);
    VmsController w;
    w.showFullScreen();
    return a.exec();
}

#include "main.moc"
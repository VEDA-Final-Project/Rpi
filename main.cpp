#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QUdpSocket>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QUrl>
#include <QDebug>
#include <QDateTime>
#include <QSocketNotifier>
#include <QTimer>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

struct CameraInfo {
    QString ip;
    QString user;
    QString pass;
};

class VmsController : public QWidget {
    Q_OBJECT

public:
    VmsController(QWidget *parent = nullptr) : QWidget(parent) {
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

        QTimer::singleShot(500, [this](){ changeChannel(1); });
    }

private:
    QUdpSocket *udpSocket;
    QMediaPlayer *player;
    QVideoWidget *videoWidget;
    QStackedWidget *contentStack; // 화면 전환용 스택
    QTableWidget *dbTable;        // DB 데이터용 테이블
    
    QMap<int, CameraInfo> cams;
    QMap<int, QPushButton*> channelButtons;
    QStringList dbTabs;
    int currentDbIdx = 0;
    int joyFd = -1;
    QSocketNotifier *joyNotifier = nullptr;
    
    // 조이스틱 상태
    bool isMovingX = false;
    bool isMovingY = false;

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
            "QPushButton#mediaBtn { border: 1px solid #3498db; color: #3498db; }"
            "QPushButton#viewSwitchBtn { border: 2px solid #e67e22; color: #e67e22; font-size: 13px; }"
            "QPushButton#dbBtn { background-color: #252a33; border: 2px solid #00ff88; font-size: 18px; }"
            "QLabel#title { color: #00ff88; font-size: 20px; font-weight: bold; }"
            "QTableWidget { background-color: #252a33; gridline-color: #3f4552; border: none; }"
            "QHeaderView::section { background-color: #2d323e; color: #00ff88; font-weight: bold; border: 1px solid #3f4552; }"
        );
    }

    void setupUI() {
        QHBoxLayout *midLayout = new QHBoxLayout(this);

        // --- 1. 왼쪽 사이드바 (개편됨) ---
        QVBoxLayout *sideBar = new QVBoxLayout();
        QLabel *titleLabel = new QLabel("VEDA NVR");
        titleLabel->setObjectName("title");
        sideBar->addWidget(titleLabel);

        // 채널 버튼 1~4
        for(int i=1; i<=4; ++i) {
            QPushButton *chBtn = new QPushButton(QString("CH %1").arg(i));
            chBtn->setObjectName("channelBtn");
            chBtn->setCheckable(true);
            chBtn->setFixedSize(140, 42);
            connect(chBtn, &QPushButton::clicked, [this, i](){ changeChannel(i); });
            sideBar->addWidget(chBtn);
            channelButtons[i] = chBtn;
        }

        sideBar->addSpacing(15);
        sideBar->addWidget(new QLabel("--- MEDIA ---"));

        // 이미지 캡처 & 영상 녹화 (복구)
        QPushButton *capBtn = new QPushButton("IMAGE CAPTURE");
        capBtn->setObjectName("mediaBtn");
        QPushButton *recBtn = new QPushButton("VIDEO RECORD");
        recBtn->setObjectName("mediaBtn");
        sideBar->addWidget(capBtn);
        sideBar->addWidget(recBtn);

        sideBar->addSpacing(10);

        // 화면 전환 버튼 (신규)
        QPushButton *viewSwitchBtn = new QPushButton("VIEW SWITCH\n[CCTV <-> DB]");
        viewSwitchBtn->setObjectName("viewSwitchBtn");
        viewSwitchBtn->setFixedSize(140, 55);
        connect(viewSwitchBtn, &QPushButton::clicked, this, &VmsController::toggleMainView);
        sideBar->addWidget(viewSwitchBtn);
        
        sideBar->addStretch();
        midLayout->addLayout(sideBar);

        // --- 2. 우측 메인 영역 (스택 구조) ---
        QVBoxLayout *mainArea = new QVBoxLayout();
        
        contentStack = new QStackedWidget(this);

        // [Page 0] 실시간 CCTV 화면
        videoWidget = new QVideoWidget();
        videoWidget->setStyleSheet("background-color: #000; border: 1px solid #333;");
        contentStack->addWidget(videoWidget);

        // [Page 1] DB 데이터 테이블 화면
        dbTable = new QTableWidget(10, 3); // 10행 3열 예시
        dbTable->setHorizontalHeaderLabels({"시간", "이벤트", "상세 내용"});
        dbTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        // 가짜 데이터 채우기 (나중에 실제 DB 쿼리 결과로 교체 가능)
        for(int r=0; r<10; ++r) {
            dbTable->setItem(r, 0, new QTableWidgetItem("19:30:12"));
            dbTable->setItem(r, 1, new QTableWidgetItem("ENTRY"));
            dbTable->setItem(r, 2, new QTableWidgetItem("차량번호 12가 3456"));
        }
        contentStack->addWidget(dbTable);

        mainArea->addWidget(contentStack, 1);

        // 하단 DB 탭 순환 버튼
        dbSwitchBtn = new QPushButton("DB TAB: [ 주차 이력 ]");
        dbSwitchBtn->setObjectName("dbBtn");
        dbSwitchBtn->setFixedHeight(65);
        connect(dbSwitchBtn, &QPushButton::clicked, this, &VmsController::handleDbSwitch);
        mainArea->addWidget(dbSwitchBtn);

        midLayout->addLayout(mainArea, 1);

        // 버튼 신호 연결
        connect(capBtn, &QPushButton::clicked, [this](){ sendCmd("CMD:CAP:NOW"); });
        connect(recBtn, &QPushButton::clicked, [this](){ sendCmd("CMD:REC:TOGGLE"); });
    }

    void setupPlayer() {
        player = new QMediaPlayer(this, QMediaPlayer::LowLatency);
        player->setVideoOutput(videoWidget);
    }

    // [핵심] 화면 전환 함수
    void toggleMainView() {
        int nextIdx = (contentStack->currentIndex() == 0) ? 1 : 0;
        contentStack->setCurrentIndex(nextIdx);
        
        QString viewName = (nextIdx == 0) ? "CCTV MODE" : "DB TABLE MODE";
        qDebug() << "[VIEW SWITCH]" << viewName;
        
        // 화면 전환 시 데스크탑에도 알려주면 좋습니다.
        sendCmd(QString("CMD:VIEW:%1").arg(nextIdx));
    }

    void changeChannel(int ch) {
        for(int i=1; i<=4; ++i) channelButtons[i]->setChecked(i == ch);
        player->stop();
        // 화면이 DB 모드여도 채널을 바꾸면 자동으로 CCTV 모드로 돌아오게 할 수도 있습니다.
        // contentStack->setCurrentIndex(0); 

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

    // --- 조이스틱/통신 (기존과 동일) ---
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
            if (ev.type == EV_KEY) {
                if (ev.value == 1) {
                    qDebug() << "[ARC BUTTON]" << "Code:" << ev.code;
                    sendCmd(QString("CMD:BTN:%1").arg(ev.code));
                }
            }
            else if (ev.type == EV_ABS) {
                if (ev.code == ABS_X) {
                    if (ev.value > 200) { sendCmd("CMD:PTZ:RIGHT"); isMovingX = true; }
                    else if (ev.value < 50) { sendCmd("CMD:PTZ:LEFT"); isMovingX = true; }
                    else if (ev.value >= 120 && ev.value <= 135) {
                        if (isMovingX) { sendCmd("CMD:PTZ:STOP"); isMovingX = false; }
                    }
                }
                else if (ev.code == ABS_Y) {
                    if (ev.value > 200) { sendCmd("CMD:PTZ:DOWN"); isMovingY = true; }
                    else if (ev.value < 50) { sendCmd("CMD:PTZ:UP"); isMovingY = true; }
                    else if (ev.value >= 120 && ev.value <= 135) {
                        if (isMovingY) { sendCmd("CMD:PTZ:STOP"); isMovingY = false; }
                    }
                }
            }
        }
    }

    void sendCmd(QString msg) {
        udpSocket->writeDatagram(msg.toUtf8(), QHostAddress(desktopIp), port);
        qDebug() << "[UDP SENT]" << msg;
    }
};

int main(int argc, char *argv[]) {
    qputenv("GST_V4L2_USE_LIBV4L2", "1");
    QApplication a(argc, argv);
    VmsController w;
    w.showFullScreen();
    return a.exec();
}

#include "main.moc"
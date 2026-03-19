#include <QApplication>
#include <QWidget>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMediaPlayer>
#include <QVideoWidget>
#include <QUrl>
#include <QDebug>
#include <QTimer>
#include <QFontDatabase>
#include <QSocketNotifier>
#include <gpiod.h> // libgpiod 헤더
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

struct CameraInfo {
    QString ip, user, pass;
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
        setupEncoder(); // libgpiod 방식
        setupTcpServer();

        dbTabs << "주차 이력" << "사용자" << "차량 정보" << "주차구역 현황";
        QTimer::singleShot(500, [this](){ changeChannel(1); });
    }

private:
    QTcpServer *tcpServer;
    QList<QTcpSocket*> clients;
    QLabel *statusDot, *statusText;
    QMediaPlayer *player;
    QVideoWidget *videoWidget;
    QStackedWidget *contentStack;
    QTableWidget *dbTable;
    QPushButton *dbSwitchBtn;

    QMap<int, CameraInfo> cams;
    QMap<int, QPushButton*> channelButtons;
    QStringList dbTabs;
    int currentCh = 1, currentDbIdx = 0;
    int joyFd = -1;
    QString lastXDir = "", lastYDir = "";

    // libgpiod 관련 변수
    struct gpiod_chip *chip = nullptr;
    struct gpiod_line_request *line_request = nullptr;
    int lastClkState = -1;
    int lastSwState = 1;

    void sendPacket(QString cmd, QString d1, QString d2 = "") {
        QString packet = QString("$%1,%2").arg(cmd).arg(d1);
        if (!d2.isEmpty()) packet += "," + d2;
        packet += "\n";
        for (QTcpSocket *c : clients) {
            if (c->state() == QAbstractSocket::ConnectedState) {
                c->write(packet.toUtf8());
                c->flush();
            }
        }
        qDebug().noquote() << "[TX]" << packet.trimmed();
    }

    void setupDesign() {
        this->setFixedSize(800, 480);
        int fontId = QFontDatabase::addApplicationFont("/home/karas/.local/share/fonts/Pretendard-Regular.otf");
        if (fontId != -1) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).at(0)));

        this->setStyleSheet(
            "QWidget { background-color: #0f1115; color: #e1e1e1; font-family: 'Pretendard'; }"
            "QPushButton#channelBtn { background-color: #1c1f26; border: 1px solid #2d323e; border-radius: 6px; padding: 10px; font-weight: 600; }"
            "QPushButton#channelBtn:checked { border: 1px solid #00ff88; color: #00ff88; }"
            "QPushButton#mediaBtn { background-color: #2980b9; border-radius: 6px; color: white; font-weight: bold; min-height: 40px; }"
            "QPushButton#viewSwitchBtn { border: 2px solid #e67e22; border-radius: 6px; color: #e67e22; font-weight: 800; font-size: 11px; }"
            "QPushButton#dbBtn { background-color: #00ff88; color: #0f1115; border-radius: 6px; font-size: 16px; font-weight: 800; }"
            "QLabel#title { color: #00ff88; font-size: 18px; font-weight: 900; }"
        );
    }

    void setupUI() {
        QVBoxLayout *rootLayout = new QVBoxLayout(this);
        rootLayout->setContentsMargins(10, 5, 10, 10);
        rootLayout->setSpacing(5);

        QHBoxLayout *header = new QHBoxLayout();
        QLabel *title = new QLabel("VEDA SMART NVR");
        title->setObjectName("title");
        statusDot = new QLabel(); statusDot->setFixedSize(12, 12);
        statusDot->setStyleSheet("background-color: #ff4d4d; border-radius: 6px;");
        statusText = new QLabel("OFFLINE");
        statusText->setStyleSheet("color: #ff4d4d; font-size: 10px; font-weight: bold;");

        header->addWidget(title); header->addStretch();
        header->addWidget(statusText); header->addWidget(statusDot);
        rootLayout->addLayout(header);

        QHBoxLayout *midLayout = new QHBoxLayout();
        QVBoxLayout *sideBar = new QVBoxLayout();
        for(int i=1; i<=4; ++i) {
            QPushButton *chBtn = new QPushButton(QString("CH 0%1").arg(i));
            chBtn->setObjectName("channelBtn"); chBtn->setCheckable(true); chBtn->setFixedSize(125, 42); 
            connect(chBtn, &QPushButton::clicked, [this, i](){ changeChannel(i); });
            sideBar->addWidget(chBtn); channelButtons[i] = chBtn;
        }

        sideBar->addSpacing(5);
        QPushButton *capBtn = new QPushButton("CAPTURE"); capBtn->setObjectName("mediaBtn");
        QPushButton *recBtn = new QPushButton("RECORD"); recBtn->setObjectName("mediaBtn");
        sideBar->addWidget(capBtn); sideBar->addWidget(recBtn);

        QPushButton *viewBtn = new QPushButton("VIEW SWITCH"); viewBtn->setObjectName("viewSwitchBtn");
        viewBtn->setFixedSize(125, 52);
        connect(viewBtn, &QPushButton::clicked, this, &VmsController::toggleMainView);
        sideBar->addWidget(viewBtn); sideBar->addStretch();
        midLayout->addLayout(sideBar);

        QVBoxLayout *mainArea = new QVBoxLayout();
        contentStack = new QStackedWidget();
        videoWidget = new QVideoWidget();
        videoWidget->setStyleSheet("background-color: black; border-radius: 8px;");
        contentStack->addWidget(videoWidget);

        dbTable = new QTableWidget(10, 3);
        dbTable->setHorizontalHeaderLabels({"TIME", "EVENT", "VALUE"});
        dbTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        contentStack->addWidget(dbTable);
        mainArea->addWidget(contentStack, 1);

        dbSwitchBtn = new QPushButton("DB TAB: [ 주차 이력 ]");
        dbSwitchBtn->setObjectName("dbBtn"); dbSwitchBtn->setFixedHeight(55);
        connect(dbSwitchBtn, &QPushButton::clicked, this, &VmsController::handleDbSwitch);
        mainArea->addWidget(dbSwitchBtn);

        midLayout->addLayout(mainArea, 1);
        rootLayout->addLayout(midLayout);

        connect(capBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "294"); });
        connect(recBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "295"); });
    }

    void setupTcpServer() {
        tcpServer = new QTcpServer(this);
        if (tcpServer->listen(QHostAddress::Any, 12345)) {
            connect(tcpServer, &QTcpServer::newConnection, [this]() {
                QTcpSocket *c = tcpServer->nextPendingConnection();
                clients.append(c);
                statusDot->setStyleSheet("background-color: #00ff88; border-radius: 6px;");
                statusText->setText("CONNECTED"); statusText->setStyleSheet("color: #00ff88; font-size: 10px; font-weight: bold;");
                connect(c, &QTcpSocket::disconnected, [this, c]() {
                    clients.removeAll(c); c->deleteLater();
                    if (clients.isEmpty()) {
                        statusDot->setStyleSheet("background-color: #ff4d4d; border-radius: 6px;");
                        statusText->setText("OFFLINE"); statusText->setStyleSheet("color: #ff4d4d; font-size: 10px; font-weight: bold;");
                    }
                });
            });
        }
    }

    // --- [핵심] libgpiod를 이용한 엔코더 설정 ---
    void setupEncoder() {
        // 1. GPIO 칩 열기 (RPi 4는 보통 /dev/gpiochip0 또는 /dev/gpiochip4)
        // 터미널에서 'gpiodetect' 명령어로 확인 가능합니다. 보통 0번입니다.
        chip = gpiod_chip_open("/dev/gpiochip0");
        if (!chip) {
            qDebug() << "GPIO Chip open failed! Try /dev/gpiochip4 if 0 fails.";
            chip = gpiod_chip_open("/dev/gpiochip4");
            if (!chip) return;
        }

        // 2. 라인 설정 (5, 6, 13번 핀)
        unsigned int offsets[] = {5, 6, 26};
        
        struct gpiod_line_settings *settings = gpiod_line_settings_new();
        gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
        
        struct gpiod_line_config *line_cfg = gpiod_line_config_new();
        gpiod_line_config_add_line_settings(line_cfg, offsets, 3, settings);

        // 3. 라인 요청 (Request)
        struct gpiod_request_config *req_cfg = gpiod_request_config_new();
        gpiod_request_config_set_consumer(req_cfg, "vms_encoder");

        line_request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);

        // 메모리 해제 (요청 후 설정 객체들은 더 이상 필요 없음)
        gpiod_line_settings_free(settings);
        gpiod_line_config_free(line_cfg);
        gpiod_request_config_free(req_cfg);

        if (!line_request) {
            qDebug() << "Line request failed!";
            return;
        }

        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &VmsController::readEncoder);
        timer->start(5);
        qDebug() << "[SYSTEM] libgpiod v2.x Encoder initialized (Pins 5, 6, 26)";
    }

    void readEncoder() {
        if (!line_request) return;

        // v2.x 방식의 값 읽기 (0: Low, 1: High)
        int c = gpiod_line_request_get_value(line_request, 5);
        int d = gpiod_line_request_get_value(line_request, 6);
        int s = gpiod_line_request_get_value(line_request, 26);

        if (lastClkState != -1 && c != lastClkState) {
            // 회전 방향 판별
            sendPacket("ENC", (d != c) ? "1" : "-1");
            qDebug() << "ENC Rotated:" << ((d != c) ? "CW" : "CCW");
        }
        lastClkState = c;

        if (s == 0 && lastSwState == 1) {
            sendPacket("ENC", "CLK");
            qDebug() << "ENC SW Pressed";
        }
        lastSwState = s;
    }

    void setupJoystick() {
        joyFd = open("/dev/input/event4", O_RDONLY | O_NONBLOCK);
        if (joyFd >= 0) {
            QSocketNotifier *n = new QSocketNotifier(joyFd, QSocketNotifier::Read, this);
            connect(n, &QSocketNotifier::activated, this, &VmsController::readJoystick);
        }
    }

    void readJoystick() {
        struct input_event ev;
        while (read(joyFd, &ev, sizeof(ev)) > 0) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_X) {
                    if (ev.value > 200) { sendPacket("JOY", "R", "1"); lastXDir = "R"; }
                    else if (ev.value < 50) { sendPacket("JOY", "L", "1"); lastXDir = "L"; }
                    else if (ev.value >= 120 && ev.value <= 135 && !lastXDir.isEmpty()) { sendPacket("JOY", lastXDir, "0"); lastXDir = ""; }
                }
                else if (ev.code == ABS_Y) {
                    if (ev.value > 200) { sendPacket("JOY", "D", "1"); lastYDir = "D"; }
                    else if (ev.value < 50) { sendPacket("JOY", "U", "1"); lastYDir = "U"; }
                    else if (ev.value >= 120 && ev.value <= 135 && !lastYDir.isEmpty()) { sendPacket("JOY", lastYDir, "0"); lastYDir = ""; }
                }
            }
            else if (ev.type == EV_KEY && ev.value == 1) sendPacket("BTN", QString::number(ev.code));
        }
    }

    void changeChannel(int ch) {
        currentCh = ch; for(int i=1; i<=4; ++i) channelButtons[i]->setChecked(i == ch);
        player->stop();
        QUrl url(QString("rtsp://%1:554/profile7/media.smp").arg(cams[ch].ip));
        url.setUserName(cams[ch].user); url.setPassword(cams[ch].pass);
        player->setMedia(url); player->play();
        sendPacket("BTN", QString::number(287 + ch));
    }

    void setupPlayer() { player = new QMediaPlayer(this, QMediaPlayer::LowLatency); player->setVideoOutput(videoWidget); }
    void toggleMainView() { contentStack->setCurrentIndex(contentStack->currentIndex() == 0 ? 1 : 0); sendPacket("BTN", "292"); }
    void handleDbSwitch() { currentDbIdx = (currentDbIdx + 1) % dbTabs.size(); dbSwitchBtn->setText(QString("DB TAB: [ %1 ]").arg(dbTabs[currentDbIdx])); sendPacket("BTN", "293"); }
};

int main(int argc, char *argv[]) {
    qputenv("GST_V4L2_USE_LIBV4L2", "1");
    QApplication a(argc, argv);
    VmsController w;
    w.showFullScreen();
    return a.exec();
}

#include "main.moc"
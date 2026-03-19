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
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFontDatabase>
#include <QSocketNotifier>
#include <gpiod.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

struct CameraInfo { QString ip, user, pass; };

class VmsController : public QWidget {
    Q_OBJECT
public:
    VmsController(QWidget *parent = nullptr) : QWidget(parent) {
        // 카메라 정보 설정
        cams[1] = {"192.168.0.23", "admin", "1team@@@"};
        cams[2] = {"192.168.0.34", "admin", "1team@@@"};
        cams[3] = {"192.168.0.76", "admin", "1team@@@"};
        cams[4] = {"192.168.0.78", "admin", "1team@@@"};
        
        dbTabNames << "주차 이력" << "사용자" << "차량 정보" << "주차구역 현황";

        // 초기화 순서
        setupDesign();
        setupUI();
        setupPlayer();
        setupTcpServer(); // TCP 서버 시작 (포트 12345)
        setupJoystick();  // 조이스틱 설정 (event4)
        setupEncoder();   // 엔코더 설정 (BCM 5, 6, 26)

        QTimer::singleShot(500, [this](){ changeChannel(1); });
    }

private:
    // UI 및 통신 멤버 변수
    QTcpServer *tcpServer;
    QList<QTcpSocket*> clients;
    QLabel *statusDot, *statusText;
    QMediaPlayer *player;
    QVideoWidget *videoWidget;
    
    QStackedWidget *mainStack;    // [0] 영상, [1] DB화면
    QStackedWidget *dbTabStack;   // DB화면 내 4개 테이블 스택
    QTableWidget* tables[4];      
    QPushButton *dbSwitchBtn;
    QStringList dbTabNames;

    QMap<int, CameraInfo> cams;
    QMap<int, QPushButton*> channelButtons;
    int currentCh = 1, currentDbIdx = 0;

    // 하드웨어 멤버 변수 (스코프 에러 방지)
    int joyFd = -1; 
    QString lastXDir = "", lastYDir = ""; 
    struct gpiod_chip *chip = nullptr;
    struct gpiod_line_request *line_req = nullptr;
    int lastClkState = -1, lastSwState = 1; // Active-Low (평소 1)
    QElapsedTimer swTimer;

    // --- [기능] 패킷 전송 ---
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

    // --- [기능] 윈도우 DB 데이터 수신 및 테이블 업데이트 ---
    void updateTableFromJson(const QByteArray &jsonData) {
        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (doc.isNull()) return;

        QJsonObject jsonObj = doc.object();
        int tIdx = jsonObj["table_idx"].toInt();
        QJsonArray dataArr = jsonObj["data"].toArray();

        if (tIdx < 0 || tIdx > 3) return;

        QTableWidget* target = tables[tIdx];
        target->setRowCount(0);

        int colCount = target->columnCount();

        for (int i = 0; i < dataArr.size(); ++i) {
            QJsonObject rowObj = dataArr[i].toObject();
            target->insertRow(i);
            for (int col = 0; col < colCount; ++col) {
                QString key = QString("col%1").arg(col + 1);
                target->setItem(i, col, new QTableWidgetItem(rowObj[key].toString()));
            }
        }
        qDebug() << "[DB] Table" << tIdx << "updated with" << dataArr.size() << "rows.";
    }

    // --- [설정] UI 디자인 ---
    void setupDesign() {
        this->setFixedSize(800, 480);
        int fontId = QFontDatabase::addApplicationFont("/home/karas/.local/share/fonts/Pretendard-Regular.otf");
        if (fontId != -1) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(fontId).at(0)));

        this->setStyleSheet(
            "QWidget { background-color: #0f1115; color: #e1e1e1; font-family: 'Pretendard'; }"
            "QPushButton#channelBtn { background-color: #1c1f26; border: 1px solid #2d323e; border-radius: 6px; padding: 10px; font-weight: 600; }"
            "QPushButton#channelBtn:checked { border: 1px solid #00ff88; color: #00ff88; }"
            "QPushButton#mediaBtn { background-color: #2980b9; border-radius: 6px; color: white; font-weight: bold; min-height: 40px; }"
            "QPushButton#liveBtn { background-color: #2ecc71; border-radius: 6px; color: white; font-weight: bold; min-height: 45px; }"
            "QPushButton#dbBtn { background-color: #00ff88; color: #0f1115; border-radius: 6px; font-size: 16px; font-weight: 800; }"
            "QLabel#title { color: #00ff88; font-size: 24px; font-weight: 900; }"
            "QTableWidget { background-color: #16191d; gridline-color: #2d323e; border: none; font-size: 10px; color: #e1e1e1; }"
            "QHeaderView::section { background-color: #2d323e; color: #00ff88; font-weight: bold; font-size: 10px; border: 1px solid #16191d; }"
        );
    }

    void setupUI() {
        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(10, 5, 10, 10);
        
        QHBoxLayout *header = new QHBoxLayout();
        QLabel *title = new QLabel("VEDA SMART NVR");
        title->setObjectName("title");
        statusDot = new QLabel(); statusDot->setFixedSize(12, 12);
        statusDot->setStyleSheet("background-color: #ff4d4d; border-radius: 6px;"); // 초기 빨간불
        statusText = new QLabel("OFFLINE");
        header->addWidget(title); header->addStretch(); header->addWidget(statusText); header->addWidget(statusDot);
        root->addLayout(header);

        QHBoxLayout *mid = new QHBoxLayout();
        QVBoxLayout *side = new QVBoxLayout();
        
        // 채널 버튼
        for(int i=1; i<=4; ++i) {
            QPushButton *btn = new QPushButton(QString("CH 0%1").arg(i));
            btn->setObjectName("channelBtn"); btn->setCheckable(true); btn->setFixedSize(125, 42);
            connect(btn, &QPushButton::clicked, [this, i](){ switchToLiveView(); changeChannel(i); });
            side->addWidget(btn); channelButtons[i] = btn;
        }
        side->addSpacing(10);
        
        // 미디어 버튼
        QPushButton *capBtn = new QPushButton("CAPTURE"); capBtn->setObjectName("mediaBtn");
        QPushButton *recBtn = new QPushButton("RECORD"); recBtn->setObjectName("mediaBtn");
        connect(capBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "294"); });
        connect(recBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "295"); });
        side->addWidget(capBtn); side->addWidget(recBtn);
        
        side->addSpacing(5);
        
        // 라이브 뷰 복귀 버튼 (RECORD 밑)
        QPushButton *liveBtn = new QPushButton("LIVE VIEW");
        liveBtn->setObjectName("liveBtn");
        connect(liveBtn, &QPushButton::clicked, this, &VmsController::switchToLiveView);
        side->addWidget(liveBtn);
        
        side->addStretch();
        mid->addLayout(side);

        QVBoxLayout *mainArea = new QVBoxLayout();
        mainStack = new QStackedWidget();
        videoWidget = new QVideoWidget();
        mainStack->addWidget(videoWidget);

        // 4개 DB 테이블 스택 설정
        dbTabStack = new QStackedWidget();
        tables[0] = new QTableWidget(0, 6); // 주차 이력
        tables[0]->setHorizontalHeaderLabels({"번호판", "구역명", "입차시간", "출차시간", "지불 여부", "총 금액"});
        tables[1] = new QTableWidget(0, 6); // 사용자
        tables[1]->setHorizontalHeaderLabels({"Chat ID", "번호판", "이름", "연락처", "카드번호", "등록일"});
        tables[2] = new QTableWidget(0, 4); // 차량 정보
        tables[2]->setHorizontalHeaderLabels({"채널", "ReID", "Obj ID", "번호판"});
        tables[3] = new QTableWidget(0, 4); // 주차구역 현황
        tables[3]->setHorizontalHeaderLabels({"카메라", "이름", "점유", "생성일"});

        for(int i=0; i<4; ++i) {
            tables[i]->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            tables[i]->setEditTriggers(QAbstractItemView::NoEditTriggers);
            dbTabStack->addWidget(tables[i]);
        }
        mainStack->addWidget(dbTabStack);
        mainArea->addWidget(mainStack, 1);

        dbSwitchBtn = new QPushButton("데이터베이스(DB) 보기");
        dbSwitchBtn->setObjectName("dbBtn"); dbSwitchBtn->setFixedHeight(60);
        connect(dbSwitchBtn, &QPushButton::clicked, this, &VmsController::handleDbControl);
        mainArea->addWidget(dbSwitchBtn);

        mid->addLayout(mainArea, 1);
        root->addLayout(mid);
    }

    // --- [설정] 네트워크 서버 ---
    void setupTcpServer() {
        tcpServer = new QTcpServer(this);
        if (tcpServer->listen(QHostAddress::Any, 12345)) {
            qDebug() << "[SERVER] Listening on 12345";
            connect(tcpServer, &QTcpServer::newConnection, [this]() {
                QTcpSocket *c = tcpServer->nextPendingConnection();
                clients.append(c);
                statusDot->setStyleSheet("background-color: #00ff88; border-radius: 6px;"); // 초록불
                statusText->setText("CONNECTED");
                
                connect(c, &QTcpSocket::readyRead, [this, c](){
                    while(c->canReadLine()){
                        QByteArray line = c->readLine().trimmed();
                        if(line.startsWith("$DB_SYNC,")) updateTableFromJson(line.mid(9));
                    }
                });
                
                connect(c, &QTcpSocket::disconnected, [this, c](){
                    clients.removeAll(c); c->deleteLater();
                    if(clients.isEmpty()) {
                        statusDot->setStyleSheet("background-color: #ff4d4d; border-radius: 6px;");
                        statusText->setText("OFFLINE");
                    }
                });

                // [라즈베리파이 코드 수정 제안]
connect(c, &QTcpSocket::readyRead, [this, c](){
    while(c->canReadLine()){
        QByteArray line = c->readLine().trimmed();
        // ★ 아래 로그를 추가해서 원본 데이터가 들어오는지 확인하세요!
        qDebug() << "[RX Source]" << line; 
        
        if(line.startsWith("$DB_SYNC,")) {
            updateTableFromJson(line.mid(9));
        } else {
            qDebug() << "[RX Unknown]" << line;
        }
    }
});
            });
        }
    }

    // --- [설정] 하드웨어 장치 ---
    void setupJoystick() {
        joyFd = open("/dev/input/event4", O_RDONLY | O_NONBLOCK);
        if (joyFd >= 0) {
            QSocketNotifier *n = new QSocketNotifier(joyFd, QSocketNotifier::Read, this);
            connect(n, &QSocketNotifier::activated, [this](){
                struct input_event ev;
                while (read(this->joyFd, &ev, sizeof(ev)) > 0) {
                    if (ev.type == EV_ABS) {
                        if (ev.code == ABS_X) {
                            if (ev.value > 200) { sendPacket("JOY", "R", "1"); this->lastXDir = "R"; }
                            else if (ev.value < 50) { sendPacket("JOY", "L", "1"); this->lastXDir = "L"; }
                            else if (ev.value >= 120 && ev.value <= 135 && !this->lastXDir.isEmpty()) { 
                                sendPacket("JOY", this->lastXDir, "0"); this->lastXDir = ""; 
                            }
                        }
                        else if (ev.code == ABS_Y) {
                            if (ev.value > 200) { sendPacket("JOY", "D", "1"); this->lastYDir = "D"; }
                            else if (ev.value < 50) { sendPacket("JOY", "U", "1"); this->lastYDir = "U"; }
                            else if (ev.value >= 120 && ev.value <= 135 && !this->lastYDir.isEmpty()) { 
                                sendPacket("JOY", this->lastYDir, "0"); this->lastYDir = ""; 
                            }
                        }
                    } else if (ev.type == EV_KEY && ev.value == 1) {
                        sendPacket("BTN", QString::number(ev.code));
                    }
                }
            });
        }
    }

    void setupEncoder() {
        chip = gpiod_chip_open("/dev/gpiochip0"); 
        if (!chip) chip = gpiod_chip_open("/dev/gpiochip4"); 
        if (!chip) return;

        unsigned int offsets[] = {5, 6, 26};
        struct gpiod_line_settings *s = gpiod_line_settings_new();
        gpiod_line_settings_set_direction(s, GPIOD_LINE_DIRECTION_INPUT);
        gpiod_line_settings_set_bias(s, GPIOD_LINE_BIAS_PULL_UP);

        struct gpiod_line_config *cfg = gpiod_line_config_new();
        gpiod_line_config_add_line_settings(cfg, offsets, 3, s);
        struct gpiod_request_config *req = gpiod_request_config_new();
        gpiod_request_config_set_consumer(req, "vms_encoder");
        
        line_req = gpiod_chip_request_lines(chip, req, cfg);
        if (line_req) lastSwState = gpiod_line_request_get_value(line_req, 26);

        gpiod_line_settings_free(s); gpiod_line_config_free(cfg); gpiod_request_config_free(req);
        
        QTimer *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &VmsController::readEncoder);
        t->start(3);
    }

    void readEncoder() {
        if (!line_req) return;
        int c = gpiod_line_request_get_value(line_req, 5);
        int d = gpiod_line_request_get_value(line_req, 6);
        int s = gpiod_line_request_get_value(line_req, 26);

        if (lastClkState != -1 && c != lastClkState) {
            sendPacket("ENC", (d != c) ? "1" : "-1");
        }
        lastClkState = c;

        if (s == 0 && lastSwState == 1) { // 눌림 (Active-Low)
            if (!swTimer.isValid() || swTimer.elapsed() > 250) {
                sendPacket("ENC", "CLK");
                if (mainStack->currentIndex() == 1) switchToLiveView(); // DB모드 탈출
                swTimer.start();
            }
        }
        lastSwState = s;
    }

    // --- [화면 전환 제어] ---
    void switchToLiveView() {
        mainStack->setCurrentIndex(0);
        dbSwitchBtn->setText("데이터베이스(DB) 보기");
        dbSwitchBtn->setStyleSheet("background-color: #00ff88; color: #0f1115;");
        //sendPacket("BTN", "287"); 
    }

    void handleDbControl() {
        if (mainStack->currentIndex() == 0) {
            mainStack->setCurrentIndex(1);
            currentDbIdx = 0;
            dbTabStack->setCurrentIndex(0);
            updateDbButtonText();
            sendPacket("BTN", "292");
        } else {
            currentDbIdx++;
            if (currentDbIdx >= 4) switchToLiveView();
            else {
                dbTabStack->setCurrentIndex(currentDbIdx);
                updateDbButtonText();
                sendPacket("BTN", "293");
            }
        }
    }

    void updateDbButtonText() {
        dbSwitchBtn->setText(QString("[%1] 탭 (다음 탭 클릭)").arg(dbTabNames[currentDbIdx]));
        dbSwitchBtn->setStyleSheet("background-color: #e67e22; color: white;"); 
    }

    void setupPlayer() { player = new QMediaPlayer(this, QMediaPlayer::LowLatency); player->setVideoOutput(videoWidget); }
    void changeChannel(int ch) {
        currentCh = ch; for(int i=1; i<=4; ++i) channelButtons[i]->setChecked(i == ch);
        player->stop();
        QUrl url(QString("rtsp://%1:554/profile7/media.smp").arg(cams[ch].ip));
        url.setUserName(cams[ch].user); url.setPassword(cams[ch].pass);
        player->setMedia(url); player->play();
        sendPacket("BTN", QString::number(287 + ch));
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
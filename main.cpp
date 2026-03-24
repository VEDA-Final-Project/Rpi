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
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>

struct CameraInfo { QString ip, user, pass; };

class VmsController : public QWidget {
    Q_OBJECT
public:
    VmsController(QWidget *parent = nullptr) : QWidget(parent) {
        // 1. 데이터 초기화
        cams[1] = {"192.168.0.23", "admin", "1team@@@"};
        cams[2] = {"192.168.0.34", "admin", "1team@@@"};
        cams[3] = {"192.168.0.76", "admin", "1team@@@"};
        cams[4] = {"192.168.0.78", "admin", "1team@@@"};
        dbTabNames << "주차 이력" << "사용자" << "차량 정보" << "주차구역 현황";

        // 2. UI 및 시스템 초기화
        setupDesign();
        setupUI();
        setupPlayer();
        setupTcpServer(); // 윈도우 통신 서버
        setupJoystick();  // 조이스틱 (/dev/input/event4)
        setupEncoder();   // 커널 드라이버 (/dev/vms_encoder)

        QTimer::singleShot(500, [this](){ changeChannel(1); });
    }

private:
    // --- 멤버 변수 (Private 영역) ---
    int encFd = -1;
    int joyFd = -1;
    QSocketNotifier *encNotifier = nullptr;
    QSocketNotifier *joyNotifier = nullptr;
    
    QTcpServer *tcpServer;
    QList<QTcpSocket*> clients;
    QLabel *statusDot, *statusText;
    QMediaPlayer *player;
    QVideoWidget *videoWidget;
    
    QStackedWidget *mainStack;    // [0] 영상, [1] DB화면
    QStackedWidget *dbTabStack;   // DB 내부 4개 탭
    QTableWidget* tables[4];      
    QPushButton *dbSwitchBtn;
    QStringList dbTabNames;

    QMap<int, CameraInfo> cams;
    QMap<int, QPushButton*> channelButtons;
    int currentCh = 1, currentDbIdx = 0;
    QString lastXDir = "", lastYDir = ""; 
    QElapsedTimer swTimer;

    // --- [기능] 패킷 전송 (라즈베리파이 -> 윈도우) ---
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

    // --- [기능] DB 수신 파싱 (윈도우 -> 라즈베리파이) ---
    void updateTableFromJson(const QByteArray &jsonData) {
        QJsonDocument doc = QJsonDocument::fromJson(jsonData);
        if (doc.isNull()) return;

        QJsonObject jsonObj = doc.object();
        int tIdx = jsonObj["table_idx"].toInt();
        QJsonArray dataArr = jsonObj["data"].toArray();

        if (tIdx < 0 || tIdx > 3) return;

        QTableWidget* target = tables[tIdx];
        target->setRowCount(0);

        for (int i = 0; i < dataArr.size(); ++i) {
            QJsonObject rowObj = dataArr[i].toObject();
            target->insertRow(i);
            for (int col = 0; col < target->columnCount(); ++col) {
                QString key = QString("col%1").arg(col + 1);
                target->setItem(i, col, new QTableWidgetItem(rowObj[key].toString()));
            }
        }
        qDebug() << "[DB] Table" << tIdx << "Updated (" << dataArr.size() << "rows)";
    }

    // --- [설정] TCP 서버 (수신 로직 통합) ---
    void setupTcpServer() {
        tcpServer = new QTcpServer(this);
        if (tcpServer->listen(QHostAddress::Any, 12345)) {
            connect(tcpServer, &QTcpServer::newConnection, [this]() {
                QTcpSocket *c = tcpServer->nextPendingConnection();
                clients.append(c);
                statusDot->setStyleSheet("background-color: #00ff88; border-radius: 6px;");
                statusText->setText("CONNECTED");

                connect(c, &QTcpSocket::readyRead, [this, c](){
                    while(c->canReadLine()){
                        QByteArray line = c->readLine().trimmed();
                        qDebug() << "[RX]" << line;
                        if(line.startsWith("$DB_SYNC,")) {
                            updateTableFromJson(line.mid(9));
                        }
                    }
                });

                connect(c, &QTcpSocket::disconnected, [this, c](){
                    clients.removeAll(c);
                    c->deleteLater();
                    if(clients.isEmpty()) {
                        statusDot->setStyleSheet("background-color: #ff4d4d; border-radius: 6px;");
                        statusText->setText("OFFLINE");
                    }
                });
            });
        }
    }

    // --- [설정] 커널 드라이버 엔코더 연동 ---
    void setupEncoder() {
        encFd = open("/dev/vms_encoder", O_RDONLY | O_NONBLOCK);
        if (encFd >= 0) {
            qDebug() << "✅ Kernel Driver (/dev/vms_encoder) Connected!";
            encNotifier = new QSocketNotifier(encFd, QSocketNotifier::Read, this);
            connect(encNotifier, &QSocketNotifier::activated, [this](){
                int val = 0;
                if (read(encFd, &val, sizeof(int)) > 0) {
                    if (val == 1) sendPacket("ENC", "1");
                    else if (val == -1) sendPacket("ENC", "-1");
                    else if (val == 100) {
                        sendPacket("ENC", "CLK");
                        if (mainStack->currentIndex() == 1) switchToLiveView();
                    }
                }
            });
        } else {
            qDebug() << "❌ Failed to open Kernel Driver!";
        }
    }

    // --- [설정] 조이스틱 연동 ---
    void setupJoystick() {
        joyFd = open("/dev/input/event4", O_RDONLY | O_NONBLOCK);
        if (joyFd >= 0) {
            joyNotifier = new QSocketNotifier(joyFd, QSocketNotifier::Read, this);
            connect(joyNotifier, &QSocketNotifier::activated, [this](){
                struct input_event ev;
                while (read(this->joyFd, &ev, sizeof(ev)) > 0) {
                    if (ev.type == EV_ABS) {
                        if (ev.code == ABS_X) {
                            if (ev.value > 200) { sendPacket("JOY", "R", "1"); lastXDir = "R"; }
                            else if (ev.value < 50) { sendPacket("JOY", "L", "1"); lastXDir = "L"; }
                            else if (ev.value >= 120 && ev.value <= 135 && !lastXDir.isEmpty()) { 
                                sendPacket("JOY", lastXDir, "0"); lastXDir = ""; 
                            }
                        } else if (ev.code == ABS_Y) {
                            if (ev.value > 200) { sendPacket("JOY", "D", "1"); lastYDir = "D"; }
                            else if (ev.value < 50) { sendPacket("JOY", "U", "1"); lastYDir = "U"; }
                            else if (ev.value >= 120 && ev.value <= 135 && !lastYDir.isEmpty()) { 
                                sendPacket("JOY", lastYDir, "0"); lastYDir = ""; 
                            }
                        }
                    } else if (ev.type == EV_KEY && ev.value == 1) {
                        sendPacket("BTN", QString::number(ev.code));
                    }
                }
            });
        }
    }

    // --- UI 및 화면 제어 로직 ---
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
        statusDot->setStyleSheet("background-color: #ff4d4d; border-radius: 6px;");
        statusText = new QLabel("OFFLINE");
        header->addWidget(title); header->addStretch(); header->addWidget(statusText); header->addWidget(statusDot);
        root->addLayout(header);

        QHBoxLayout *mid = new QHBoxLayout();
        QVBoxLayout *side = new QVBoxLayout();
        
        for(int i=1; i<=4; ++i) {
            QPushButton *btn = new QPushButton(QString("CH 0%1").arg(i));
            btn->setObjectName("channelBtn"); btn->setCheckable(true); btn->setFixedSize(125, 42);
            connect(btn, &QPushButton::clicked, [this, i](){ switchToLiveView(); changeChannel(i); });
            side->addWidget(btn); channelButtons[i] = btn;
        }
        side->addSpacing(10);
        
        QPushButton *capBtn = new QPushButton("CAPTURE"); capBtn->setObjectName("mediaBtn");
        QPushButton *recBtn = new QPushButton("RECORD"); recBtn->setObjectName("mediaBtn");
        connect(capBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "294"); });
        connect(recBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "295"); });
        side->addWidget(capBtn); side->addWidget(recBtn);
        
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

        dbTabStack = new QStackedWidget();
        tables[0] = new QTableWidget(0, 6); tables[0]->setHorizontalHeaderLabels({"번호판", "구역명", "입차시간", "출차시간", "지불 여부", "총 금액"});
        tables[1] = new QTableWidget(0, 6); tables[1]->setHorizontalHeaderLabels({"Chat ID", "번호판", "이름", "연락처", "카드번호", "등록일"});
        tables[2] = new QTableWidget(0, 4); tables[2]->setHorizontalHeaderLabels({"채널", "ReID", "Obj ID", "번호판"});
        tables[3] = new QTableWidget(0, 4); tables[3]->setHorizontalHeaderLabels({"카메라", "이름", "점유", "생성일"});

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

    void switchToLiveView() {
        mainStack->setCurrentIndex(0);
        dbSwitchBtn->setText("데이터베이스(DB) 보기");
        dbSwitchBtn->setStyleSheet("background-color: #00ff88; color: #0f1115;");
        sendPacket("BTN", "287"); 
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
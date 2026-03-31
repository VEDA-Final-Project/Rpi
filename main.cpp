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
#include <QScrollArea>
#include <QScrollBar>

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

    QScrollArea *videoScrollArea;
    double currentZoom = 1.0;   //PTZ 변수
    
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
                statusDot->setStyleSheet("min-width: 14px; max-width: 14px; min-height: 14px; max-height: 14px; border-radius: 7px; background-color: #00ff88; border: 1px solid #00ff88; margin-top: 1px;");
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
                    statusDot->setStyleSheet("min-width: 14px; max-width: 14px; min-height: 14px; max-height: 14px; border-radius: 7px; background-color: #ff4d4d; border: 1px solid #ff4d4d; margin-top: 1px;");
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
            qDebug() << "Kernel Driver (/dev/vms_encoder) Connected!";
            encNotifier = new QSocketNotifier(encFd, QSocketNotifier::Read, this);
            connect(encNotifier, &QSocketNotifier::activated, [this](){
                int val = 0;
                if (read(encFd, &val, sizeof(int)) > 0) {
                    if (val == 1) {
                        sendPacket("ENC", "1");
                        //handleZoom(0.2); //줌 인 (20%씩 확대) 
                    }
                    else if (val == -1) {
                        sendPacket("ENC", "-1");
                        //handleZoom(-0.2); //줌 아웃 (20%씩 축소)
                    }
                    else if (val == 100) {
                        sendPacket("ENC", "CLK");
                        if (mainStack->currentIndex() == 1) switchToLiveView();
                        else resetPTZ();
                    }
                }
            });
        } else {
            qDebug() << "Failed to open Kernel Driver!";
        }
    }

    // --- 조이스틱 연동 (플러딩 완벽 방어형 상태 머신 적용) ---
    void setupJoystick() {
        joyFd = open("/dev/vms_joystick", O_RDONLY | O_NONBLOCK);
        if (joyFd >= 0) {
            joyNotifier = new QSocketNotifier(joyFd, QSocketNotifier::Read, this);
            connect(joyNotifier, &QSocketNotifier::activated, [this](){
                struct input_event ev;
                while (read(this->joyFd, &ev, sizeof(ev)) > 0) {
                    if (ev.type == EV_ABS) {
                        if (ev.code == ABS_X) {
                            QString newXDir = "";
                            if (ev.value > 200) { newXDir = "R"; handlePan(30, 0); } // 오른쪽 이동
                            else if (ev.value < 50) { newXDir = "L"; handlePan(-30, 0); }

                            // 상태가 변했을 때만 패킷 전송 (플러딩 방지)
                            if (newXDir != lastXDir) {
                                if (!lastXDir.isEmpty()) sendPacket("JOY", lastXDir, "0"); // 이전 방향 Release
                                if (!newXDir.isEmpty()) sendPacket("JOY", newXDir, "1");   // 새 방향 Press
                                lastXDir = newXDir;
                            }
                        } else if (ev.code == ABS_Y) {
                            QString newYDir = "";
                            if (ev.value > 200) { newYDir = "D"; handlePan(0, 30); }
                            else if (ev.value < 50) { newYDir = "U"; handlePan(0, -30); }

                            if (newYDir != lastYDir) {
                                if (!lastYDir.isEmpty()) sendPacket("JOY", lastYDir, "0"); // 이전 방향 Release
                                if (!newYDir.isEmpty()) sendPacket("JOY", newYDir, "1");   // 새 방향 Press
                                lastYDir = newYDir;
                            }
                        }
                    } else if (ev.type == EV_KEY && ev.value == 1) {
                        sendPacket("BTN", QString::number(ev.code));

                        // 하드웨어 버튼으로 채널 전환하는 부분 (288~291)
                        if (ev.code >= 288 && ev.code <= 291) {
                            int targetCh = ev.code - 287; // 288->1, 289->2...
                            switchToLiveView();
                            changeChannel(targetCh);
                        }
                    }
                }
            });
        }
    }

    // --- UI 및 화면 제어 로직 ---
    void setupDesign() {
        this->setFixedSize(800, 480);
        int fontId = QFontDatabase::addApplicationFont("/home/karas/.local/share/fonts/Pretendard-Regular.otf");
        QString fontFamily = fontId != -1 ? QFontDatabase::applicationFontFamilies(fontId).at(0) : "sans-serif";
        QApplication::setFont(QFont(fontFamily));

        // 최신 트렌드의 Dark Theme + RoadSide 포인트 컬러(#00d188) 적용
        this->setStyleSheet(QString(
            "QWidget { background-color: #14171c; color: #ffffff; font-family: '%1'; }" // 전체 배경
            
            // 타이틀 (RoadSide)
            "QLabel#title { color: #ffffff; font-size: 26px; font-weight: 900; letter-spacing: 1px; }"
            "QLabel#logoIcon { background: transparent; }"
            "QLabel#statusText { font-size: 13px; font-weight: 800; color: #a0a5b1; }"
            
            // 사이드바 컨테이너
            "QWidget#sidebar { background-color: #1e222a; border-radius: 12px; }"
            
            // 채널 버튼
            "QPushButton#channelBtn { background-color: #272c36; color: #a0a5b1; border: 2px solid transparent; border-radius: 8px; padding: 10px; font-size: 14px; font-weight: 800; text-align: left; padding-left: 15px; }"
            "QPushButton#channelBtn:checked { background-color: #1a2f28; border: 2px solid #00d188; color: #00d188; }"
            "QPushButton#channelBtn:pressed { background-color: #1f242c; }"
            
            // 캡처/녹화 버튼
            "QPushButton#mediaBtn { background-color: #3b4252; color: #d8dee9; border-radius: 8px; font-weight: 800; font-size: 13px; min-height: 40px; text-align: left; padding-left: 15px; }"
            "QPushButton#mediaBtn:pressed { background-color: #2e3440; }"
            
            // 라이브 뷰 버튼
            "QPushButton#liveBtn { background-color: #00d188; color: #14171c; border-radius: 8px; font-weight: 900; font-size: 14px; min-height: 45px; }"
            "QPushButton#liveBtn:pressed { background-color: #00b374; }"
            
            // DB 버튼
            "QPushButton#dbBtn { background-color: #2d3442; color: #00d188; border: 2px solid #00d188; border-radius: 10px; font-size: 16px; font-weight: 900; text-align: center; }"            "QPushButton#dbBtn:pressed { background-color: #1a2f28; }"
            
            // DB 테이블 
            "QTableWidget { background-color: #1e222a; gridline-color: #2d3442; border: 1px solid #2d3442; border-radius: 8px; font-size: 12px; color: #e1e1e1; outline: none; }"
            "QHeaderView::section { background-color: #272c36; color: #00d188; font-weight: 800; font-size: 12px; border: none; border-right: 1px solid #2d3442; border-bottom: 1px solid #2d3442; padding: 6px; }"
            "QTableWidget::item:selected { background-color: #00d188; color: #14171c; }"
        ).arg(fontFamily));
    }

    void setupUI() {
        QVBoxLayout *root = new QVBoxLayout(this);
        root->setContentsMargins(15, 15, 15, 15);
        root->setSpacing(15);
        
        // --- [헤더 영역] ---
        QHBoxLayout *header = new QHBoxLayout();
        header->setContentsMargins(5, 0, 5, 0);
        
        QHBoxLayout *titleLayout = new QHBoxLayout();
        QLabel *logoIcon = new QLabel(); logoIcon->setObjectName("logoIcon");
        QPixmap logoPixmap("/home/karas/qt_vms/icons/roadside_mark.png");
        if (!logoPixmap.isNull()) {
            logoIcon->setPixmap(logoPixmap.scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        } else {
            logoIcon->setText("X"); logoIcon->setStyleSheet("font-size: 24px; background: transparent;"); // 이미지 로드 실패 시 이모지 대체
        }
        QLabel *title = new QLabel("RoadSide"); title->setObjectName("title"); // ★ 타이틀 텍스트 "RoadSide"로 설정
        titleLayout->addWidget(logoIcon);
        titleLayout->addWidget(title);
        titleLayout->setSpacing(10);
        
        statusDot = new QLabel(); statusDot->setFixedSize(14, 14);
        statusDot->setStyleSheet("min-width: 14px; max-width: 14px; min-height: 14px; max-height: 14px; border-radius: 7px; background-color: #ff4d4d; border: 1px solid #ff4d4d; margin-top: 1px;");
        statusText = new QLabel("OFFLINE"); statusText->setObjectName("statusText");
        
        header->addLayout(titleLayout);
        header->addStretch();
        header->addWidget(statusText);
        header->addSpacing(5);
        header->addWidget(statusDot);
        root->addLayout(header);

        // --- [중앙 영역] ---
        QHBoxLayout *mid = new QHBoxLayout();
        mid->setSpacing(15);
        
        // 사이드바 (독립된 패널 디자인 적용)
        QWidget *sidebarWidget = new QWidget();
        sidebarWidget->setObjectName("sidebar");
        sidebarWidget->setFixedWidth(150);
        QVBoxLayout *side = new QVBoxLayout(sidebarWidget);
        side->setContentsMargins(12, 15, 12, 15);
        side->setSpacing(10);
        
        QLabel *chLabel = new QLabel("CHANNELS");
        chLabel->setStyleSheet("color: #6a7181; font-size: 11px; font-weight: bold; background: transparent;");
        side->addWidget(chLabel);

        QIcon channelIcon("/home/karas/qt_vms/icons/icon_channel.png"); // ★ 채널 버튼 PNG 아이콘 이미지 사용 (가상 경로)
        for(int i=1; i<=4; ++i) {
            QPushButton *btn = new QPushButton(QString("CH 0%1").arg(i));
            btn->setObjectName("channelBtn"); btn->setCheckable(true); btn->setFixedHeight(45);
            if (!channelIcon.isNull()) btn->setIcon(channelIcon); // 이미지 로드 성공 시 아이콘 설정
            connect(btn, &QPushButton::clicked, [this, i](){ switchToLiveView(); changeChannel(i); });
            side->addWidget(btn); channelButtons[i] = btn;
        }
        
        side->addSpacing(10);
        QLabel *ctrlLabel = new QLabel("CONTROLS");
        ctrlLabel->setStyleSheet("color: #6a7181; font-size: 11px; font-weight: bold; background: transparent;");
        side->addWidget(ctrlLabel);

        QIcon captureIcon("/home/karas/qt_vms/icons/icon_capture.png"); // ★ 캡처 버튼 PNG 아이콘 이미지 사용 (가상 경로)
        QIcon recordIcon("/home/karas/qt_vms/icons/icon_record.png"); // ★ 녹화 버튼 PNG 아이콘 이미지 사용 (가상 경로)
        QPushButton *capBtn = new QPushButton("CAPTURE"); capBtn->setObjectName("mediaBtn");
        QPushButton *recBtn = new QPushButton("RECORD"); recBtn->setObjectName("mediaBtn");
        if (!captureIcon.isNull()) capBtn->setIcon(captureIcon); // 이미지 로드 성공 시 아이콘 설정
        if (!recordIcon.isNull()) recBtn->setIcon(recordIcon); // 이미지 로드 성공 시 아이콘 설정
        connect(capBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "294"); });
        connect(recBtn, &QPushButton::clicked, [this](){ sendPacket("BTN", "295"); });
        side->addWidget(capBtn); side->addWidget(recBtn);
        
        side->addStretch();
        
        QPushButton *liveBtn = new QPushButton("▶ LIVE VIEW"); // ★ "▶" 이모지 아이콘은 깨지지 않으므로 유지
        liveBtn->setObjectName("liveBtn");
        connect(liveBtn, &QPushButton::clicked, this, &VmsController::switchToLiveView);
        side->addWidget(liveBtn);
        
        mid->addWidget(sidebarWidget);

        // 메인 영상 및 DB 영역
        QVBoxLayout *mainArea = new QVBoxLayout();
        mainArea->setSpacing(15);
        
        mainStack = new QStackedWidget();
        mainStack->setStyleSheet("background-color: #000000; border-radius: 12px;");
        videoWidget = new QVideoWidget();
        
        //비디오 위젯을 스크롤 에어리어로 감싸기 (소프트웨어 PTZ 용도)
        videoScrollArea = new QScrollArea();
        videoScrollArea->setWidget(videoWidget);
        videoScrollArea->setWidgetResizable(true); // 기본은 화면에 꽉 차게
        videoScrollArea->setAlignment(Qt::AlignCenter);
        videoScrollArea->setStyleSheet("border: none; background-color: transparent;");
        videoScrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // 스크롤바 숨김
        videoScrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        
        mainStack->addWidget(videoScrollArea);

        dbTabStack = new QStackedWidget();
        tables[0] = new QTableWidget(0, 6); tables[0]->setHorizontalHeaderLabels({"번호판", "구역명", "입차시간", "출차시간", "지불 여부", "총 금액"});
        tables[1] = new QTableWidget(0, 6); tables[1]->setHorizontalHeaderLabels({"Chat ID", "번호판", "이름", "연락처", "카드번호", "등록일"});
        tables[2] = new QTableWidget(0, 4); tables[2]->setHorizontalHeaderLabels({"채널", "ReID", "Obj ID", "번호판"});
        tables[3] = new QTableWidget(0, 4); tables[3]->setHorizontalHeaderLabels({"카메라", "이름", "점유", "생성일"});

        for(int i=0; i<4; ++i) {
            tables[i]->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
            tables[i]->setEditTriggers(QAbstractItemView::NoEditTriggers);
            tables[i]->setSelectionBehavior(QAbstractItemView::SelectRows);
            tables[i]->verticalHeader()->setVisible(false); // ★ 엑셀 같은 행 번호 숨김 (깔끔한 대시보드 느낌)
            tables[i]->setShowGrid(false); // 내부 선 제거
            dbTabStack->addWidget(tables[i]);
        }
        mainStack->addWidget(dbTabStack);
        mainArea->addWidget(mainStack, 1);

        QIcon dbIcon("/home/karas/qt_vms/icons/icon_db.png"); //DB 버튼 PNG 아이콘 이미지 사용 (가상 경로)
        dbSwitchBtn = new QPushButton("데이터베이스(DB) 보기");
        dbSwitchBtn->setObjectName("dbBtn"); dbSwitchBtn->setFixedHeight(55);
        if (!dbIcon.isNull()) dbSwitchBtn->setIcon(dbIcon); // 이미지 로드 성공 시 아이콘 설정
        connect(dbSwitchBtn, &QPushButton::clicked, this, &VmsController::handleDbControl);
        mainArea->addWidget(dbSwitchBtn);

        mid->addLayout(mainArea, 1);
        root->addLayout(mid);
    }

    void switchToLiveView() {
        mainStack->setCurrentIndex(0);
        dbSwitchBtn->setText("데이터베이스(DB) 보기");
        dbSwitchBtn->setStyleSheet(""); // QSS 기본 스타일로 복구
        sendPacket("BTN", "288"); 
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
        // DB 보기 활성화 시 버튼 디자인 변경
        dbSwitchBtn->setStyleSheet("background-color: #1a2f28; color: #00d188; border: 2px solid #00d188;"); 
    }

    // --- 소프트웨어 PTZ 제어 ---
    void handleZoom(double delta) {
        if (mainStack->currentIndex() != 0) return; // 라이브 뷰 화면일 때만 작동
        
        currentZoom += delta;
        if (currentZoom < 1.0) currentZoom = 1.0;
        if (currentZoom > 5.0) currentZoom = 5.0; // 최대 5배 줌 제한

        if (currentZoom == 1.0) {
            videoScrollArea->setWidgetResizable(true); // 1배율이면 원래 화면 꽉 차게 복귀
        } else {
            videoScrollArea->setWidgetResizable(false);
            QSize baseSize = videoScrollArea->viewport()->size();
            videoWidget->setFixedSize(baseSize * currentZoom); // 배율만큼 크기 강제 확장
        }
    }

    void resetPTZ() {
        currentZoom = 1.0;
        videoScrollArea->setWidgetResizable(true);
    }

    void handlePan(int dx, int dy) {
        if (currentZoom <= 1.0 || mainStack->currentIndex() != 0) return; // 확대 안 되어있으면 이동 안 함
        QScrollBar *hBar = videoScrollArea->horizontalScrollBar();
        QScrollBar *vBar = videoScrollArea->verticalScrollBar();
        hBar->setValue(hBar->value() + dx);
        vBar->setValue(vBar->value() + dy);
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
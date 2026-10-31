// Stream Time - OBS plugin for streamtime.ir
// Login, auto-configure stream key, and start/stop streaming + local recording together.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/config-file.h>
#include <plugin-support.h>

#include <QWidget>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <cstring>
#include <QFile>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static const char *API_BASE = "https://streamtime.ir";
static const char *RTMP_URL = "rtmp://streamtime.ir/live";

// ---- token storage (Windows DPAPI, stored in OBS user config) ----
static QByteArray protect(const QByteArray &in, bool enc)
{
#ifdef _WIN32
	DATA_BLOB i{(DWORD)in.size(), (BYTE *)in.data()}, o{};
	BOOL ok = enc ? CryptProtectData(&i, L"StreamTime", nullptr, nullptr, nullptr, 0, &o)
		      : CryptUnprotectData(&i, nullptr, nullptr, nullptr, nullptr, 0, &o);
	if (!ok)
		return {};
	QByteArray r((const char *)o.pbData, (int)o.cbData);
	LocalFree(o.pbData);
	return r;
#else
	(void)enc;
	return in;
#endif
}

static QString cfgPath()
{
	char *p = obs_module_config_path("token.dat");
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

static void saveToken(const QString &t)
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);
	QFile f(cfgPath());
	if (t.isEmpty()) {
		f.remove();
		return;
	}
	if (f.open(QIODevice::WriteOnly))
		f.write(protect(t.toUtf8(), true).toBase64());
}

static QString loadToken()
{
	QFile f(cfgPath());
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QString::fromUtf8(protect(QByteArray::fromBase64(f.readAll()), false));
}

// ---- independent video pipeline (own resolution/fps, same aspect as OBS canvas) ----
struct Pipe {
	obs_view_t *view = nullptr;
	video_t *video = nullptr;
	obs_source_t *src = nullptr;
};

static bool openPipe(Pipe &p, int height, int fps, int &outW)
{
	obs_video_info ovi;
	if (!obs_get_video_info(&ovi))
		return false;
	double aspect = (double)ovi.base_width / (double)ovi.base_height;
	int w = ((int)(height * aspect + 0.5) / 2) * 2;
	int h = (height / 2) * 2;
	// Input canvas == output size: one single resolution.
	ovi.base_width = ovi.output_width = (uint32_t)w;
	ovi.base_height = ovi.output_height = (uint32_t)h;
	ovi.fps_num = (uint32_t)fps;
	ovi.fps_den = 1;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	p.view = obs_view_create();
	p.video = obs_view_add2(p.view, &ovi);
	if (!p.video) {
		obs_view_destroy(p.view);
		p.view = nullptr;
		return false;
	}
	outW = w;
	return true;
}

static void syncScene(Pipe &p)
{
	if (!p.view)
		return;
	obs_source_t *s = obs_frontend_get_current_scene();
	if (s != p.src) {
		obs_view_set_source(p.view, 0, s);
		p.src = s;
	}
	obs_source_release(s);
}

static void closePipe(Pipe &p)
{
	if (!p.view)
		return;
	obs_view_set_source(p.view, 0, nullptr);
	obs_view_remove(p.view);
	obs_view_destroy(p.view);
	p = Pipe();
}

static QString settingsPath()
{
	char *p = obs_module_config_path("settings.json");
	QString s = QString::fromUtf8(p);
	bfree(p);
	return s;
}

// ---- dock ----
class StreamTimeDock : public QWidget {
	QNetworkAccessManager net;
	QString token, streamKey;
	QLineEdit *email, *pass, *folder;
	QPushButton *loginBtn, *logoutBtn, *startBtn, *recStartBtn, *browseBtn;
	QComboBox *sRes, *sFps, *rRes, *rFps, *rFmt;
	QSpinBox *sBr, *rBr;
	QCheckBox *useMain;
	QWidget *sBox, *rBox, *rCustom;
	QLabel *status, *plan;
	bool active = false, recActive = false;
	int recCounter = 1;
	std::atomic<bool> outStopped{false}, recStopped{false};
	obs_output_t *out = nullptr, *rout = nullptr;
	obs_encoder_t *venc = nullptr, *aenc = nullptr, *rvenc = nullptr, *raenc = nullptr;
	obs_service_t *svc = nullptr;
	Pipe spipe, rpipe;

	static void fillRes(QComboBox *c, int maxH)
	{
		c->clear();
		for (int h : {480, 720, 1080, 1440, 2160})
			if (h <= maxH)
				c->addItem(QString("%1p").arg(h), h);
	}
	static void selectData(QComboBox *c, int v)
	{
		int i = c->findData(v);
		if (i >= 0)
			c->setCurrentIndex(i);
	}

public:
	StreamTimeDock()
	{
		auto *l = new QVBoxLayout(this);
		email = new QLineEdit;
		email->setPlaceholderText("Email");
		pass = new QLineEdit;
		pass->setPlaceholderText("Password");
		pass->setEchoMode(QLineEdit::Password);
		loginBtn = new QPushButton("Login");
		logoutBtn = new QPushButton("Logout");
		plan = new QLabel;
		status = new QLabel;
		status->setWordWrap(true);
		l->addWidget(email);
		l->addWidget(pass);
		l->addWidget(loginBtn);
		l->addWidget(plan);

		// Stream settings
		sBox = new QGroupBox("Stream settings");
		auto *sf = new QFormLayout(sBox);
		sRes = new QComboBox;
		fillRes(sRes, 720);
		sFps = new QComboBox;
		rFps = new QComboBox;
		for (int f : {24, 30, 48, 60}) {
			sFps->addItem(QString::number(f), f);
			rFps->addItem(QString::number(f), f);
		}
		selectData(sFps, 30);
		selectData(rFps, 30);
		sBr = new QSpinBox;
		sBr->setRange(500, 2000);
		sBr->setValue(2000);
		sBr->setSuffix(" kbps");
		sf->addRow("Resolution", sRes);
		sf->addRow("FPS", sFps);
		sf->addRow("Bitrate", sBr);
		l->addWidget(sBox);
		startBtn = new QPushButton("📡 Stream");
		l->addWidget(startBtn);

		// Record settings
		rBox = new QGroupBox("Record settings");
		auto *rv = new QVBoxLayout(rBox);
		useMain = new QCheckBox("Use OBS main recording settings");
		rv->addWidget(useMain);
		rCustom = new QWidget;
		auto *rf = new QFormLayout(rCustom);
		rf->setContentsMargins(0, 0, 0, 0);
		rRes = new QComboBox;
		fillRes(rRes, 2160);
		selectData(rRes, 1080);
		rBr = new QSpinBox;
		rBr->setRange(500, 100000);
		rBr->setValue(8000);
		rBr->setSuffix(" kbps");
		rFmt = new QComboBox;
		rFmt->addItem("mkv");
		rFmt->addItem("mp4");
		folder = new QLineEdit(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/StreamTime");
		browseBtn = new QPushButton("...");
		auto *fh = new QHBoxLayout;
		fh->addWidget(folder);
		fh->addWidget(browseBtn);
		rf->addRow("Resolution", rRes);
		rf->addRow("FPS", rFps);
		rf->addRow("Bitrate", rBr);
		rf->addRow("Format", rFmt);
		rf->addRow("Folder", fh);
		rv->addWidget(rCustom);
		l->addWidget(rBox);
		recStartBtn = new QPushButton("⏺ Record");
		l->addWidget(recStartBtn);
		l->addWidget(logoutBtn);
		l->addWidget(status);
		l->addStretch();

		loadSettings();

		connect(loginBtn, &QPushButton::clicked, this, [this] { login(); });
		connect(logoutBtn, &QPushButton::clicked, this, [this] {
			token.clear();
			streamKey.clear();
			saveToken("");
			refreshUi();
		});
		connect(browseBtn, &QPushButton::clicked, this, [this] {
			QString d = QFileDialog::getExistingDirectory(this, "Recording folder", folder->text());
			if (!d.isEmpty())
				folder->setText(d);
		});
		connect(startBtn, &QPushButton::clicked, this, [this] {
			if (out)
				obs_output_stop(out);
			else
				start();
		});
		connect(recStartBtn, &QPushButton::clicked, this, [this] {
			if (rout)
				obs_output_stop(rout);
			else
				startRecording();
		});
		auto save = [this] { saveSettings(); refreshUi(); };
		for (auto *c : {sRes, sFps, rRes, rFps, rFmt})
			connect(c, &QComboBox::currentIndexChanged, this, save);
		for (auto *s : {sBr, rBr})
			connect(s, &QSpinBox::editingFinished, this, save);
		connect(folder, &QLineEdit::editingFinished, this, save);
		connect(useMain, &QCheckBox::toggled, this, save);

		token = loadToken();
		refreshUi();
		if (!token.isEmpty())
			loadAccount();

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] {
			if (out && outStopped)
				cleanup();
			if (rout && recStopped)
				cleanupRec();
			active = out != nullptr;
			recActive = rout != nullptr;
			syncScene(spipe);
			syncScene(rpipe);
			refreshUi();
		});
		timer->start(500);
	}

	~StreamTimeDock()
	{
		if (out)
			obs_output_force_stop(out);
		if (rout)
			obs_output_force_stop(rout);
		cleanup();
		cleanupRec();
	}

private:
	void loadSettings()
	{
		QFile f(settingsPath());
		if (!f.open(QIODevice::ReadOnly))
			return;
		auto o = QJsonDocument::fromJson(f.readAll()).object();
		selectData(sRes, o["sRes"].toInt(720));
		selectData(sFps, o["sFps"].toInt(30));
		sBr->setValue(o["sBr"].toInt(2000));
		selectData(rRes, o["rRes"].toInt(1080));
		selectData(rFps, o["rFps"].toInt(30));
		rBr->setValue(o["rBr"].toInt(8000));
		rFmt->setCurrentText(o["rFmt"].toString("mkv"));
		if (o.contains("folder"))
			folder->setText(o["folder"].toString());
		useMain->setChecked(o["useMain"].toBool(false));
		recCounter = qMax(1, o["counter"].toInt(1));
	}

	void saveSettings()
	{
		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		QJsonObject o{{"sRes", sRes->currentData().toInt()}, {"sFps", sFps->currentData().toInt()},
			      {"sBr", sBr->value()},                 {"rRes", rRes->currentData().toInt()},
			      {"rFps", rFps->currentData().toInt()}, {"rBr", rBr->value()},
			      {"rFmt", rFmt->currentText()},         {"folder", folder->text()},
			      {"useMain", useMain->isChecked()},     {"counter", recCounter}};
		QFile f(settingsPath());
		if (f.open(QIODevice::WriteOnly))
			f.write(QJsonDocument(o).toJson());
	}

	static void onOutStop(void *p, calldata_t *) { ((StreamTimeDock *)p)->outStopped = true; }
	static void onRecStop(void *p, calldata_t *) { ((StreamTimeDock *)p)->recStopped = true; }

	static QString btnStyle(bool on)
	{
		return QString("QPushButton{background:%1;color:white;font-weight:bold;padding:8px;border-radius:4px;}"
			       "QPushButton:disabled{background:#666;}")
			.arg(on ? "#2e9e4f" : "#c0392b");
	}

	void refreshUi()
	{
		bool in = !token.isEmpty();
		email->setVisible(!in);
		pass->setVisible(!in);
		loginBtn->setVisible(!in);
		logoutBtn->setVisible(in && !active);
		sBox->setVisible(in);
		sBox->setEnabled(!active);
		startBtn->setVisible(in);
		startBtn->setEnabled(active || !streamKey.isEmpty());
		startBtn->setStyleSheet(btnStyle(active));
		// Record is local only: usable even before login
		rBox->setEnabled(!recActive);
		rCustom->setEnabled(!useMain->isChecked());
		recStartBtn->setStyleSheet(btnStyle(recActive));
	}

	void cleanup()
	{
		outStopped = false;
		obs_output_release(out);
		obs_encoder_release(venc);
		obs_encoder_release(aenc);
		obs_service_release(svc);
		out = nullptr;
		venc = aenc = nullptr;
		svc = nullptr;
		closePipe(spipe);
	}

	void cleanupRec()
	{
		recStopped = false;
		obs_output_release(rout);
		obs_encoder_release(rvenc);
		obs_encoder_release(raenc);
		rout = nullptr;
		rvenc = raenc = nullptr;
		closePipe(rpipe);
	}

	QNetworkRequest req(const char *path)
	{
		QNetworkRequest r(QUrl(QString(API_BASE) + path));
		r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
		if (!token.isEmpty())
			r.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
		return r;
	}

	void login()
	{
		QJsonObject b{{"email", email->text().trimmed()}, {"password", pass->text()}};
		status->setText("Logging in...");
		auto *rep = net.post(req("/api/auth/login"), QJsonDocument(b).toJson());
		connect(rep, &QNetworkReply::finished, this, [this, rep] {
			rep->deleteLater();
			QByteArray body = rep->readAll();
			int code = rep->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			auto o = QJsonDocument::fromJson(body).object();
			QString t = o.value("token").toString();
			if (t.isEmpty())
				t = o.value("access_token").toString();
			if (t.isEmpty()) {
				QString d = o.value("detail").toString();
				if (d.isEmpty())
					d = QString("HTTP %1 err=%2 body=%3")
						    .arg(code)
						    .arg(rep->errorString())
						    .arg(QString::fromUtf8(body.left(150)));
				status->setText(d);
				return;
			}
			token = t;
			saveToken(t);
			pass->clear();
			status->clear();
			refreshUi();
			loadAccount();
		});
	}

	void loadAccount()
	{
		auto *k = net.get(req("/api/stream/my-key"));
		connect(k, &QNetworkReply::finished, this, [this, k] {
			k->deleteLater();
			if (k->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 401) {
				token.clear();
				saveToken("");
				status->setText("Session expired. Please login again.");
				refreshUi();
				return;
			}
			auto o = QJsonDocument::fromJson(k->readAll()).object();
			streamKey = o["streamtime_key"].toString();
			if (streamKey.isEmpty())
				status->setText("No stream key yet. Create one on streamtime.ir.");
			refreshUi();
		});
		auto *s = net.get(req("/api/user/subscription"));
		connect(s, &QNetworkReply::finished, this, [this, s] {
			s->deleteLater();
			auto o = QJsonDocument::fromJson(s->readAll()).object();
			// Plan caps (server also enforces bitrate)
			QString p = o["plan"].toString();
			bool paid = o["has_subscription"].toBool();
			if (!paid)
				p = "free";
			int mx = p == "diamond" ? 6000 : p == "gold" ? 3500 : p == "silver" ? 2500 : 2000;
			int mh = p == "diamond" ? 2160 : (p == "gold" || p == "silver") ? 1080 : 720;
			int curH = sRes->currentData().toInt();
			int curBr = sBr->value();
			fillRes(sRes, mh);
			selectData(sRes, qMin(curH ? curH : mh, mh));
			sBr->setRange(500, mx);
			sBr->setValue(qMin(curBr, mx));
			plan->setText(paid ? QString("Plan: %1 (%2 days left) - max %3p, %4 kbps")
						     .arg(p).arg(o["days_left"].toInt()).arg(mh).arg(mx)
					   : QString("Plan: Free - max %1p, %2 kbps").arg(mh).arg(mx));
		});
	}

	void start()
	{
		auto *u = net.get(req("/api/stream/usage"));
		connect(u, &QNetworkReply::finished, this, [this, u] {
			u->deleteLater();
			auto o = QJsonDocument::fromJson(u->readAll()).object();
			if (o["limit_reached"].toBool()) {
				status->setText("Daily limit reached. Try tomorrow or upgrade your plan.");
				return;
			}
			cleanup();
			int w = 0, h = sRes->currentData().toInt(), fps = sFps->currentData().toInt();
			if (!openPipe(spipe, h, fps, w)) {
				status->setText("Could not create video pipeline.");
				return;
			}
			syncScene(spipe);

			// Independent output: does NOT touch OBS's own stream settings/key.
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "server", RTMP_URL);
			obs_data_set_string(d, "key", streamKey.toUtf8().constData());
			svc = obs_service_create("rtmp_custom", "streamtime_svc", d, nullptr);
			obs_data_release(d);

			obs_data_t *vs = obs_data_create();
			obs_data_set_string(vs, "rate_control", "CBR");
			obs_data_set_int(vs, "bitrate", sBr->value());
			obs_data_set_int(vs, "keyint_sec", 2);
			obs_data_set_string(vs, "preset", "veryfast");
			venc = obs_video_encoder_create("obs_x264", "streamtime_v", vs, nullptr);
			obs_data_release(vs);
			obs_encoder_set_video(venc, spipe.video);

			obs_data_t *as = obs_data_create();
			obs_data_set_int(as, "bitrate", 128);
			aenc = obs_audio_encoder_create("ffmpeg_aac", "streamtime_a", as, 0, nullptr);
			obs_data_release(as);
			obs_encoder_set_audio(aenc, obs_get_audio());

			out = obs_output_create("rtmp_output", "streamtime_out", nullptr, nullptr);
			obs_output_set_service(out, svc);
			obs_output_set_video_encoder(out, venc);
			obs_output_set_audio_encoder(out, aenc, 0);
			outStopped = false;
			signal_handler_connect(obs_output_get_signal_handler(out), "stop", onOutStop, this);
			if (obs_output_start(out))
				status->setText(QString("Streaming %1x%2 @ %3 fps").arg(w).arg(h).arg(fps));
			else {
				status->setText(QString("Failed: ") + obs_output_get_last_error(out));
				cleanup();
			}
		});
	}

	// Independent local recording: own output, encoders, resolution/fps; saved on user's PC.
	void startRecording()
	{
		QString dir = folder->text(), ext = rFmt->currentText();
		int h = rRes->currentData().toInt(), fps = rFps->currentData().toInt(), br = rBr->value();
		bool main = useMain->isChecked();
		video_t *video = nullptr;
		cleanupRec();
		if (main) {
			// Take everything from OBS's own recording settings
			config_t *cfg = obs_frontend_get_profile_config();
			const char *mode = cfg ? config_get_string(cfg, "Output", "Mode") : nullptr;
			bool adv = mode && !strcmp(mode, "Advanced");
			const char *sec = adv ? "AdvOut" : "SimpleOutput";
			const char *p = cfg ? config_get_string(cfg, sec, adv ? "RecFilePath" : "FilePath") : nullptr;
			const char *f = cfg ? config_get_string(cfg, sec, "RecFormat2") : nullptr;
			if (!f && cfg)
				f = config_get_string(cfg, sec, "RecFormat");
			if (p && *p)
				dir = QString::fromUtf8(p);
			QString fs = f ? QString::fromUtf8(f) : "mkv";
			ext = fs.contains("mp4") ? "mp4" : fs == "mov" ? "mov" : "mkv";
			br = (!adv && cfg) ? (int)config_get_uint(cfg, "SimpleOutput", "VBitrate") : 0;
			video = obs_get_video();
		} else {
			int w = 0;
			if (!openPipe(rpipe, h, fps, w)) {
				status->setText("Could not create video pipeline.");
				return;
			}
			syncScene(rpipe);
			video = rpipe.video;
		}
		QDir().mkpath(dir);
		QString path;
		do {
			path = QString("%1/StreamTime_%2.%3").arg(dir).arg(recCounter, 4, 10, QChar('0')).arg(ext);
			if (!QFile::exists(path))
				break;
			recCounter++;
		} while (true);

		obs_data_t *vs = obs_data_create();
		obs_data_set_string(vs, "preset", "veryfast");
		if (br > 0) {
			obs_data_set_string(vs, "rate_control", "CBR");
			obs_data_set_int(vs, "bitrate", br);
		} else {
			obs_data_set_string(vs, "rate_control", "CRF");
			obs_data_set_int(vs, "crf", 20);
		}
		rvenc = obs_video_encoder_create("obs_x264", "streamtime_rv", vs, nullptr);
		obs_data_release(vs);
		obs_encoder_set_video(rvenc, video);

		obs_data_t *as = obs_data_create();
		obs_data_set_int(as, "bitrate", 192);
		raenc = obs_audio_encoder_create("ffmpeg_aac", "streamtime_ra", as, 0, nullptr);
		obs_data_release(as);
		obs_encoder_set_audio(raenc, obs_get_audio());

		obs_data_t *os = obs_data_create();
		obs_data_set_string(os, "path", path.toUtf8().constData());
		if (ext == "mp4") // crash-safe fragmented mp4
			obs_data_set_string(os, "muxer_settings", "movflags=frag_keyframe+empty_moov");
		rout = obs_output_create("ffmpeg_muxer", "streamtime_rec", os, nullptr);
		obs_data_release(os);
		obs_output_set_video_encoder(rout, rvenc);
		obs_output_set_audio_encoder(rout, raenc, 0);
		recStopped = false;
		signal_handler_connect(obs_output_get_signal_handler(rout), "stop", onRecStop, this);
		if (obs_output_start(rout)) {
			recCounter++; // next recording gets the next number
			saveSettings();
			status->setText("Recording to: " + path);
		} else {
			status->setText(QString("Recording failed: ") + obs_output_get_last_error(rout));
			cleanupRec();
		}
	}
};

bool obs_module_load(void)
{
	obs_frontend_add_dock_by_id("streamtime_dock", "Stream Time", new StreamTimeDock());
	obs_log(LOG_INFO, "Stream Time plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void) {}

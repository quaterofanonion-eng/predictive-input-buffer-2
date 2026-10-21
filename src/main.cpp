// Predictive Input Buffer - Geode mod for Geometry Dash (2.2081)  v4.7
//
// Every jump-button release is held back until the click has lasted at least N
// physics ticks (1 tick = 1/240 s). Presses are never delayed.
//
//   STAGE 1: 2 ticks  All | 2 ticks Wave
//   STAGE 2: 4 ticks  All | 4 ticks Wave
//   STAGE 3: 8 ticks  All | 8 ticks Wave
//   STAGE 4: 16 ticks All | 8 ticks Wave
//   STAGE 5: OFF (100% raw input)
//
// F6 (in a level) cycles the stage. F7 prints session stats.
//
// v4.7 changes
//   - epsilon on due-time comparisons (float accumulation could add a tick)
//   - pending releases are delivered before a reset / on quit instead of dropped
//   - failsafe raised to 1500 ms so slow-motion practice does not cut windows short
//   - per-session stats (clicks / padded / assisted) and F7 summary
//   - optional JSONL event log for the external console (console.py)

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/CCKeyboardDispatcher.hpp>
#include <chrono>
#include <fstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace ib {

	using Clock = std::chrono::steady_clock;

	constexpr double kTick = 1.0 / 240.0;
	constexpr double kEps = 1e-6;
	constexpr int kJump = 1;                // PlayerButton::Jump
	constexpr int kFailsafeMs = 1500;       // never hold a deferred release longer than this (real time)

	struct Stage {
		int all;
		int wave;
	};

	constexpr int kStageCount = 5;
	constexpr Stage kStages[kStageCount] = {
		{ 2, 2 },
		{ 4, 4 },
		{ 8, 8 },
		{ 16, 8 },
		{ 0, 0 }    // OFF
	};

	struct Slot {
		bool tracking = false;      // a press was delivered and not yet released
		bool pending = false;       // the release is being held back
		bool early = false;         // the press started while airborne
		bool assisted = false;      // player landed while the release was held back
		double pressedAt = 0.0;
		double releaseAt = 0.0;
		double rawReleaseAt = 0.0;  // when the player actually let go
		Clock::time_point deferredAt{};
	};

	struct State {
		double clock = 0.0;
		bool bypass = false;        // pass handleButton straight through (internal flushes)
		Slot slot[2];
	};

	struct Stats {
		unsigned clicks = 0;
		unsigned padded = 0;
		unsigned assisted = 0;
	};

	inline State g;
	inline Stats stats;
	inline int stage = 0;

	inline bool flag(char const* key) {
		return Mod::get()->getSettingValue<bool>(key);
	}

	inline void resetState() {
		g = State{};
	}

	// ---- external log (JSON lines, written outside the input path) -------------------------

	inline std::vector<std::string> lines;
	inline std::ofstream out;
	inline bool opened = false;

	inline std::filesystem::path logPath() {
		return Mod::get()->getSaveDir() / "events.jsonl";
	}

	inline long long wallMs() {
		using namespace std::chrono;
		return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	}

	inline void emit(std::string s) {
		if (!flag("external-log")) return;
		lines.push_back(std::move(s));
		if (lines.size() > 4096) lines.erase(lines.begin(), lines.begin() + 2048);
	}

	inline void flushLog() {
		if (lines.empty()) return;
		if (!opened) {
			out.open(logPath(), std::ios::trunc);   // fresh file each session
			opened = true;
		}
		if (!out) { lines.clear(); return; }
		for (auto const& l : lines) out << l << '\n';
		out.flush();
		lines.clear();
	}

	// ---- helpers ---------------------------------------------------------------------------

	inline std::string describe(int idx) {
		auto const& s = kStages[idx];
		if (s.all == 0) {
			return fmt::format("STAGE {}: OFF (100% Raw Input)", idx + 1);
		}
		double allF = s.all / 4.0;
		double waveF = s.wave / 4.0;
		if (s.all == s.wave) {
			return fmt::format(
				"STAGE {}: {} Ticks All | {} Ticks Wave ({:.1f}f @ 60fps)",
				idx + 1, s.all, s.wave, allF
			);
		}
		return fmt::format(
			"STAGE {}: {} Ticks All | {} Ticks Wave ({:.1f}f / {:.1f}f @ 60fps)",
			idx + 1, s.all, s.wave, allF, waveF
		);
	}

	inline bool wanted() {
		return flag("enabled") && kStages[stage].all > 0;
	}

	inline bool isPlayLayer(GJBaseGameLayer* layer) {
		auto pl = PlayLayer::get();
		return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
	}

	inline char const* modeName(PlayerObject* p) {
		if (!p) return "unknown";
		if (p->m_isDart) return "wave";
		if (p->m_isShip) return "ship";
		if (p->m_isBird) return "ufo";
		if (p->m_isSwing) return "swing";
		if (p->m_isBall) return "ball";
		if (p->m_isRobot) return "robot";
		if (p->m_isSpider) return "spider";
		return "cube";
	}

	inline int ticksFor(PlayerObject* p, bool early) {
		auto const& st = kStages[stage];
		if (!p) return 0;
		if (p->m_isDart) {
			return flag("buffer-wave") ? st.wave : 0;
		}
		if (p->m_isShip || p->m_isBird || p->m_isSwing) {
			return flag("buffer-flight-modes") ? st.all : 0;
		}
		if (flag("only-when-airborne") && !early) return 0;
		return st.all;
	}

	// Record one delivered release (padded or not) in the stats and the event log.
	inline void logRelease(int s, PlayerObject* p, Slot const& sl, bool padded, double deliveredClock) {
		double rawHold = ((padded ? sl.rawReleaseAt : deliveredClock) - sl.pressedAt) / kTick;
		double finalHold = (deliveredClock - sl.pressedAt) / kTick;
		++stats.clicks;
		if (padded) {
			++stats.padded;
			if (sl.assisted) ++stats.assisted;
		}
		emit(fmt::format(
			R"({{"t":{},"ev":"release","slot":{},"stage":{},"mode":"{}","early":{},"padded":{},"assisted":{},"raw_ticks":{:.2f},"final_ticks":{:.2f}}})",
			wallMs(), s, stage + 1, modeName(p), sl.early, padded, padded && sl.assisted, rawHold, finalHold
		));
	}

	inline void banner() {
		std::string sep(60, '=');
		log::info("{}", sep);
		log::info("{}", " [InputBuffer] v4.7 LOADED");
		log::info("{}", " [InputBuffer] Starting Mode: STAGE 1 (2 All / 2 Wave)");
		log::info("{}", " [InputBuffer] F6 in-level: cycle 2/2 -> 4/4 -> 8/8 -> 16/8 -> OFF | F7: session stats");
		log::info(" [InputBuffer] External log file: {}", logPath().string());
		log::info("{}", sep);
	}

	inline void cycleStage() {
		stage = (stage + 1) % kStageCount;
		auto text = describe(stage);
		log::info("[InputBuffer] F6 Cycle -> {}", text);
		emit(fmt::format(R"({{"t":{},"ev":"stage","stage":{}}})", wallMs(), stage + 1));
		if (flag("show-toast")) {
			Notification::create(
				fmt::format("Input Buffer: {}", text),
				NotificationIcon::None,
				1.5f
			)->show();
		}
	}

	inline void printStats() {
		double pct = stats.clicks ? 100.0 * stats.padded / stats.clicks : 0.0;
		double apct = stats.padded ? 100.0 * stats.assisted / stats.padded : 0.0;
		auto text = fmt::format(
			"clicks {} | padded {} ({:.0f}%) | assisted {} ({:.0f}% of padded)",
			stats.clicks, stats.padded, pct, stats.assisted, apct
		);
		log::info("[InputBuffer] Session stats: {}", text);
		if (flag("show-toast")) {
			Notification::create(text, NotificationIcon::None, 2.5f)->show();
		}
	}

} // namespace ib

$on_mod(Loaded) {
	ib::banner();
}

class $modify(IBGameLayer, GJBaseGameLayer) {

	PlayerObject* ibPlayer(int s) {
		PlayerObject* p = s == 0 ? m_player1 : m_player2;
		return p ? p : m_player1;
	}

	// Deliver a release that was being held back.
	void ibRelease(int s) {
		auto& sl = ib::g.slot[s];
		ib::logRelease(s, this->ibPlayer(s), sl, true, ib::g.clock);
		sl.pending = false;
		sl.tracking = false;
		GJBaseGameLayer::handleButton(false, ib::kJump, s == 0);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (ib::g.bypass) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}
		if (button != ib::kJump || !ib::wanted() || !ib::isPlayLayer(this)) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int s = isPlayer1 ? 0 : 1;
		auto& sl = ib::g.slot[s];
		PlayerObject* p = this->ibPlayer(s);

		if (down) {
			if (sl.pending) this->ibRelease(s);

			sl.tracking = true;
			sl.assisted = false;
			sl.early = p ? !p->m_isOnGround : false;
			sl.pressedAt = ib::g.clock;
			GJBaseGameLayer::handleButton(true, button, isPlayer1);
			return;
		}

		// Release of a click we did not see being pressed (e.g. the stage was just switched on).
		if (!sl.tracking) {
			GJBaseGameLayer::handleButton(false, button, isPlayer1);
			return;
		}

		int ticks = ib::ticksFor(p, sl.early);
		double due = sl.pressedAt + ticks * ib::kTick;

		if (ticks <= 0 || ib::g.clock + ib::kEps >= due) {
			ib::logRelease(s, p, sl, false, ib::g.clock);
			sl.tracking = false;
			sl.pending = false;
			GJBaseGameLayer::handleButton(false, button, isPlayer1);
			return;
		}

		sl.pending = true;
		sl.releaseAt = due;
		sl.rawReleaseAt = ib::g.clock;
		sl.deferredAt = ib::Clock::now();

		if (ib::flag("debug-log")) {
			double heldTicks = (ib::g.clock - sl.pressedAt) / ib::kTick;
			log::info(
				"[InputBuffer] release deferred: held {:.1f} ticks -> padded to {} ticks (early={}, mode={})",
				heldTicks, ticks, sl.early, ib::modeName(p)
			);
		}
	}

	// Runs once per physics step: advance our clock and deliver releases that are due.
	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (ib::isPlayLayer(this)) {
			ib::g.clock += dt;
			bool wanted = ib::wanted();
			for (int s = 0; s < 2; ++s) {
				auto& sl = ib::g.slot[s];
				if (!sl.pending) continue;

				// Landed while the release was held back: the padding likely mattered.
				PlayerObject* p = this->ibPlayer(s);
				if (p && sl.early && p->m_isOnGround) sl.assisted = true;

				if (!wanted || ib::g.clock + ib::kEps >= sl.releaseAt) {
					this->ibRelease(s);
				}
			}
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	// Failsafe + log flushing (file IO stays out of the input path).
	void update(float dt) {
		GJBaseGameLayer::update(dt);
		if (!ib::isPlayLayer(this)) return;

		auto now = ib::Clock::now();
		for (int s = 0; s < 2; ++s) {
			auto& sl = ib::g.slot[s];
			if (sl.pending && now - sl.deferredAt > std::chrono::milliseconds(ib::kFailsafeMs)) {
				this->ibRelease(s);
			}
		}
		ib::flushLog();
	}
};

namespace {
	// Deliver any held-back releases before the level state is torn down.
	void flushPending(PlayLayer* layer) {
		for (int s = 0; s < 2; ++s) {
			if (!ib::g.slot[s].pending) continue;
			ib::logRelease(s, s == 0 ? layer->m_player1 : layer->m_player2, ib::g.slot[s], true, ib::g.clock);
			ib::g.bypass = true;
			layer->handleButton(false, ib::kJump, s == 0);
			ib::g.bypass = false;
		}
	}
}

class $modify(IBPlayLayer, PlayLayer) {
	void resetLevel() {
		flushPending(this);
		PlayLayer::resetLevel();
		ib::resetState();
	}

	void onQuit() {
		flushPending(this);
		ib::resetState();
		ib::flushLog();
		PlayLayer::onQuit();
	}
};

class $modify(IBKeyboard, CCKeyboardDispatcher) {
	bool dispatchKeyboardMSG(enumKeyCodes key, bool down, bool repeat, double timestamp) {
		if (down && !repeat && PlayLayer::get()) {
			if (key == enumKeyCodes::KEY_F6) ib::cycleStage();
			else if (key == enumKeyCodes::KEY_F7) ib::printStats();
		}
		return CCKeyboardDispatcher::dispatchKeyboardMSG(key, down, repeat, timestamp);
	}
};

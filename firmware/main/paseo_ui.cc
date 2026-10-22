// Phase-2/3 screens: overview / result / permissions / command menu /
// voice confirm. See paseo_ui.h for the threading rules and the phase-3
// key model. Rendering is deliberately text-mode (one label per region,
// lines pre-truncated/wrapped here): on a 240x320 panel with a few
// hundred KB of RAM this is the robust shape, and navigation state
// stays trivially consistent.
#include "paseo_ui.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

#include "display.h"
#include "paseo_app.h"

#define TAG "paseo_ui"

/* Display width budget in terminal-style units: ASCII counts 1, wide
 * (multibyte) glyphs count 2. Tuned for the default theme font on the
 * 240 px panel; lines are clipped by the label as a backstop. */
static const int kLineUnits = 26;
static const int kListRows = 11;   /* visible rows on list screens */
static const int kTextRows = 12;   /* visible lines on text screens */
static const int kMaxWrappedLines = 200;

/* Hold OK at least this long and the press becomes a voice take
 * (README_PHASE3 key model); a shorter press is a normal OK click. */
static const int kVoiceHoldMs = 350;
/* On the confirm screen, ignore OK for this long so the framework
 * CLICK trailing a recording release cannot send by accident. */
static const int kConfirmOkGuardMs = 400;
static const int kConfirmTimeoutMs = 10000;

/* Preset commands (phase-2 scope item 5). The strings are sent verbatim
 * as the message text / initial prompt. */
static const char *kPresetContinue = "继续";
static const char *kPresetStatus = "汇报当前状态";
static const char *kPresetTest = "跑一遍测试";
static const char *kPresetNewTaskPrompt = "汇报当前状态";

static const char *kMenuItems[] = {"继续", "汇报当前状态", "跑一遍测试",
                                   "停止当前任务", "新建任务", "返回"};
static const int kMenuCount = 6;

static int64_t NowMs() { return esp_timer_get_time() / 1000; }

/* Display width of one UTF-8 codepoint starting at p (bytes consumed in
 * *used). Invalid bytes count as width 1 and consume 1 byte. */
static int CpWidth(const char *p, int *used)
{
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) { *used = 1; return 1; }
    if ((c & 0xE0) == 0xC0) { *used = 2; return 2; }
    if ((c & 0xF0) == 0xE0) { *used = 3; return 2; }
    if ((c & 0xF8) == 0xF0) { *used = 4; return 2; }
    *used = 1;
    return 1;
}

/* Copy src into out truncated to max_units display width, never splitting
 * a UTF-8 sequence; control chars become spaces. */
static std::string Truncate(const std::string &src, int max_units)
{
    std::string out;
    int w = 0;
    for (size_t i = 0; i < src.size();) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c == 0x7f) { out += ' '; i++; continue; }
        int used;
        int cw = CpWidth(src.c_str() + i, &used);
        if (w + cw > max_units) break;
        out.append(src, i, used);
        w += cw;
        i += used;
    }
    return out;
}

/* Wrap s into lines of at most kLineUnits display width (hard cap
 * kMaxWrappedLines lines). Newlines force a break. */
static void WrapTextInto(const std::string &s,
                         std::vector<std::string> &lines)
{
    lines.clear();
    std::string cur;
    int cur_w = 0;
    auto flush = [&]() {
        lines.push_back(cur);
        cur.clear();
        cur_w = 0;
    };
    for (size_t i = 0; i < s.size() && (int)lines.size() < kMaxWrappedLines;) {
        char c = s[i];
        if (c == '\n') { flush(); i++; continue; }
        if (c == '\r') { i++; continue; }
        int used;
        int cw = CpWidth(s.c_str() + i, &used);
        if (cur_w + cw > kLineUnits && !cur.empty()) flush();
        if ((unsigned char)c >= 0x20) cur.append(s, i, used);
        cur_w += cw;
        i += used;
    }
    if (!cur.empty() || lines.empty()) flush();
}

/* ---------------- lifecycle ---------------- */

void PaseoUi::Init(Display *display, const std::string &def_provider,
                   const std::string &def_cwd,
                   const std::string &asr_endpoint,
                   const std::string &asr_model, const std::string &asr_key)
{
    display_ = display;
    if (!def_provider.empty()) def_provider_ = def_provider;
    def_cwd_ = def_cwd;
    voice_.SetAsrConfig(asr_endpoint, asr_model, asr_key);
    voice_ready_ = voice_.Init();
    if (!voice_ready_)
        ESP_LOGW(TAG, "voice engine unavailable (storage mount failed)");
}

void PaseoUi::Show()
{
    if (!display_) return;
    if (overlay_) { Render(); return; }
    /* Creation runs under the display lock; the lock is released before
     * the trailing Render() call, which takes the lock itself. */
    {
    DisplayLockGuard lock(display_);
    if (!lock) return;
    auto *scr = lv_screen_active();
    lv_obj_t *ov = lv_obj_create(scr);
    lv_obj_set_size(ov, display_->width(), display_->height());
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_set_style_bg_color(ov, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);

    lv_obj_t *head = lv_label_create(ov);
    lv_obj_set_pos(head, 4, 2);
    lv_obj_set_width(head, display_->width() - 8);
    lv_label_set_long_mode(head, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(head, lv_color_hex(0x9fd8a8), 0);

    lv_obj_t *body = lv_label_create(ov);
    lv_obj_set_pos(body, 4, 26);
    lv_obj_set_width(body, display_->width() - 8);
    lv_obj_set_height(body, display_->height() - 26 - 44);
    lv_label_set_long_mode(body, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(body, lv_color_hex(0xf0f0f0), 0);

    lv_obj_t *foot = lv_label_create(ov);
    lv_obj_set_pos(foot, 4, display_->height() - 42);
    lv_obj_set_width(foot, display_->width() - 8);
    lv_obj_set_height(foot, 40);
    lv_label_set_long_mode(foot, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(foot, lv_color_hex(0xa0a0a0), 0);

    lv_obj_move_foreground(ov);
    overlay_ = ov;
    head_label_ = head;
    body_label_ = body;
    foot_label_ = foot;
    } /* display lock released */
    dirty_ = true;
    Render();
}

void PaseoUi::Hide()
{
    /* Abandon any voice take in flight; its result is ignored later
     * because voice_ui_ is reset (PollResult drops stale results). */
    if (voice_ui_ != V_NONE) voice_.Cancel();
    voice_ui_ = V_NONE;
    ok_swallow_click_ = false;
    if (!overlay_) return;
    DisplayLockGuard lock(display_);
    if (lock) lv_obj_del((lv_obj_t *)overlay_);
    overlay_ = head_label_ = body_label_ = foot_label_ = nullptr;
}

/* ---------------- client callback sinks (client task) ---------------- */

void PaseoUi::OnState(paseo_state_t st)
{
    conn_ = st;
    dirty_ = true;
}

void PaseoUi::OnAgents(const paseo_agent_t *agents, int count)
{
    if (count > PASEO_MAX_AGENTS) count = PASEO_MAX_AGENTS;
    memcpy(agents_, agents, sizeof(paseo_agent_t) * count);
    agent_count_ = count;
    if (focus_ >= count) focus_ = count > 0 ? count - 1 : 0;
    dirty_ = true;
}

void PaseoUi::OnPerms(const paseo_perm_t *perms, int count)
{
    if (count > PASEO_MAX_PERMS) count = PASEO_MAX_PERMS;
    memcpy(perms_, perms, sizeof(paseo_perm_t) * count);
    perm_count_ = count;
    if (perm_focus_ > count) perm_focus_ = count;
    if (resolving_id_[0]) {
        bool still = false;
        for (int i = 0; i < count; i++)
            if (strcmp(perms_[i].id, resolving_id_) == 0) still = true;
        if (!still) {
            resolving_id_[0] = '\0';
            SetFeedback("审批已生效");
            if (screen_ == SCR_PERMS && count == 0) screen_ = SCR_OVERVIEW;
        }
    }
    dirty_ = true;
}

void PaseoUi::OnTimeline(const char *agent_id, const char *text,
                         const char *error)
{
    if (!agent_id || strcmp(agent_id, result_agent_) != 0) return;
    result_loading_ = false;
    result_error_ = error ? error : "";
    result_text_ = text ? text : "";
    scroll_ = 0;
    RebuildWrappedText();
    dirty_ = true;
}

void PaseoUi::OnAction(paseo_action_t action, bool ok, const char *agent_id,
                       const char *error)
{
    (void)agent_id;
    switch (action) {
    case PASEO_ACT_SEND_MESSAGE:
        if (ok) SetFeedback("指令已受理");
        else SetFeedback("发送失败：%s", error ? error : "?");
        break;
    case PASEO_ACT_CREATE_AGENT:
        if (ok) {
            SetFeedback("已新建 agent");
            screen_ = SCR_OVERVIEW;
        } else {
            SetFeedback("新建失败：%s", error ? error : "?");
        }
        break;
    case PASEO_ACT_CANCEL_AGENT:
        if (ok) SetFeedback("已发送停止");
        else SetFeedback("停止失败：%s", error ? error : "?");
        break;
    }
    dirty_ = true;
}

/* ---------------- helpers ---------------- */

void PaseoUi::SetFeedback(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    feedback_ = buf;
    feedback_until_ms_ = NowMs() + 6000;
}

const paseo_agent_t *PaseoUi::FocusedAgent() const
{
    if (agent_count_ == 0) return nullptr;
    int i = focus_;
    if (i < 0) i = 0;
    if (i >= agent_count_) i = agent_count_ - 1;
    return &agents_[i];
}

static std::string AgentLabel(const paseo_agent_t &a)
{
    if (a.title[0]) return a.title;
    return std::string(a.id, 0, 8);
}

void PaseoUi::RebuildWrappedText() { WrapTextInto(result_text_, wrapped_); }

/* ---------------- navigation / actions ---------------- */

void PaseoUi::EnterResult()
{
    const paseo_agent_t *a = FocusedAgent();
    if (!a) { SetFeedback("暂无 agent"); return; }
    strncpy(result_agent_, a->id, sizeof(result_agent_) - 1);
    result_agent_[sizeof(result_agent_) - 1] = '\0';
    screen_ = SCR_RESULT;
    result_loading_ = true;
    result_error_.clear();
    result_text_.clear();
    wrapped_.clear();
    scroll_ = 0;
    if (!paseo_client_fetch_timeline(result_agent_)) {
        result_loading_ = false;
        result_error_ = "未连接";
    }
    dirty_ = true;
}

void PaseoUi::EnterMenu()
{
    strncpy(menu_agent_, result_agent_, sizeof(menu_agent_) - 1);
    menu_agent_[sizeof(menu_agent_) - 1] = '\0';
    menu_return_ = SCR_RESULT;
    menu_focus_ = 0;
    screen_ = SCR_MENU;
    dirty_ = true;
}

void PaseoUi::ExecMenuItem(int idx)
{
    switch (idx) {
    case 0:
    case 1:
    case 2: {
        const char *text = idx == 0   ? kPresetContinue
                           : idx == 1 ? kPresetStatus
                                      : kPresetTest;
        if (!paseo_client_send_message(menu_agent_, text)) {
            SetFeedback("发送失败（未连接）");
            break;
        }
        SetFeedback("发送中：%s", kMenuItems[idx]);
        screen_ = menu_return_;
        /* Refresh the timeline so the agent's reply shows up here. */
        result_loading_ = true;
        if (!paseo_client_fetch_timeline(result_agent_))
            result_loading_ = false;
        break;
    }
    case 3:
        if (!paseo_client_cancel_agent(menu_agent_)) {
            SetFeedback("发送失败（未连接）");
            break;
        }
        SetFeedback("发送中：停止");
        screen_ = menu_return_;
        break;
    case 4:
        if (def_cwd_.empty()) {
            SetFeedback("未配置默认 cwd（配网页填写）");
            break;
        }
        if (!paseo_client_create_agent(def_provider_.c_str(),
                                       def_cwd_.c_str(),
                                       kPresetNewTaskPrompt)) {
            SetFeedback("发送失败（未连接）");
            break;
        }
        SetFeedback("正在新建 agent…");
        break;
    case 5:
        screen_ = menu_return_;
        break;
    }
    dirty_ = true;
}

void PaseoUi::ResolvePerm(bool allow)
{
    if (perm_focus_ < 0 || perm_focus_ >= perm_count_) return;
    const paseo_perm_t &pm = perms_[perm_focus_];
    if (resolving_id_[0] && strcmp(resolving_id_, pm.id) == 0) {
        SetFeedback("该请求处理中…");
        return;
    }
    if (!paseo_client_permission_response(pm.agent_id, pm.id, allow)) {
        SetFeedback("发送失败（未连接）");
        return;
    }
    strncpy(resolving_id_, pm.id, sizeof(resolving_id_) - 1);
    resolving_id_[sizeof(resolving_id_) - 1] = '\0';
    resolving_since_ms_ = NowMs();
    SetFeedback(allow ? "已批准，等待生效…" : "已驳回，等待生效…");
    dirty_ = true;
}

/* ---------------- phase 3: voice flow ----------------
 * Gesture state machine (app task). A press of OK on the overview or
 * result screen while ONLINE is "tracked": Tick() promotes it to a
 * recording once it has been held kVoiceHoldMs; releasing it earlier
 * synthesizes the screen's normal OK click by hand and the framework
 * CLICK that follows is swallowed exactly once. A press that became a
 * recording (or had to be aborted) never fires a click at all. */

void PaseoUi::OnPressDown(int key)
{
    int64_t now = NowMs();
    if (key == PASEO_KEY_UP || key == PASEO_KEY_DOWN) {
        /* The only nav-key meaning during a take is "cancel". */
        if (voice_ui_ == V_REC || voice_ui_ == V_UPLOAD) {
            voice_.Cancel();
            voice_ui_ = V_NONE;
            nav_ignore_until_ms_ = now + 800;
            SetFeedback(voice_.State() == PV_RECORDING ? "录音已取消"
                                                       : "已取消识别");
            dirty_ = true;
        }
        return;
    }
    if (key != PASEO_KEY_OK) return;
    if (voice_ui_ != V_NONE) return; /* a take is already in progress */
    bool armed = (screen_ == SCR_OVERVIEW || screen_ == SCR_RESULT) &&
                 conn_ == PASEO_ST_ONLINE;
    if (!armed) return; /* other screens: OK stays a plain click/long key */
    voice_ui_ = V_PENDING;
    voice_from_ = screen_;
    ok_down_ms_ = now;
    dirty_ = true;
}

void PaseoUi::OnPressUp(int key)
{
    if (key != PASEO_KEY_OK) return;
    int64_t now = NowMs();
    switch (voice_ui_) {
    case V_PENDING:
        /* Released before the hold threshold: a normal OK click, done
         * by hand; swallow the framework CLICK for this same press. */
        voice_ui_ = V_NONE;
        ok_swallow_click_ = true;
        ok_swallow_until_ms_ = now + 1500;
        SynthOkClick();
        break;
    case V_ABORT:
        /* Consumed press (recording could not start): swallow the
         * framework CLICK, do nothing else. */
        voice_ui_ = V_NONE;
        ok_swallow_click_ = true;
        ok_swallow_until_ms_ = now + 1500;
        break;
    case V_REC:
        voice_.StopAndTranscribe();
        voice_ui_ = V_UPLOAD;
        dirty_ = true;
        break;
    default:
        break; /* V_NONE / V_UPLOAD: nothing bound to OK release */
    }
}

void PaseoUi::SynthOkClick()
{
    /* Only overview/result presses are ever tracked, and on those
     * screens OK click means: overview -> result, result -> menu. */
    if (screen_ == SCR_OVERVIEW) EnterResult();
    else if (screen_ == SCR_RESULT) EnterMenu();
}

void PaseoUi::EnterConfirm(const char *text)
{
    confirm_text_ = text ? text : "";
    WrapTextInto(confirm_text_, confirm_lines_);
    confirm_create_ = false;
    confirm_agent_[0] = '\0';
    if (voice_from_ == SCR_RESULT && result_agent_[0]) {
        strncpy(confirm_agent_, result_agent_, sizeof(confirm_agent_) - 1);
        confirm_agent_[sizeof(confirm_agent_) - 1] = '\0';
    } else if (const paseo_agent_t *a = FocusedAgent()) {
        strncpy(confirm_agent_, a->id, sizeof(confirm_agent_) - 1);
        confirm_agent_[sizeof(confirm_agent_) - 1] = '\0';
    } else if (!def_provider_.empty() && !def_cwd_.empty()) {
        confirm_create_ = true; /* no agent: create one with this prompt */
    }
    confirm_deadline_ms_ = NowMs() + kConfirmTimeoutMs;
    confirm_ok_guard_ms_ = NowMs() + kConfirmOkGuardMs;
    screen_ = SCR_CONFIRM;
    dirty_ = true;
}

void PaseoUi::SendConfirmed()
{
    bool queued;
    if (confirm_create_) {
        queued = paseo_client_create_agent(def_provider_.c_str(),
                                           def_cwd_.c_str(),
                                           confirm_text_.c_str());
        if (!queued) { SetFeedback("发送失败（未连接）"); dirty_ = true; return; }
        SetFeedback("发送中：语音新建任务");
    } else if (confirm_agent_[0]) {
        queued = paseo_client_send_message(confirm_agent_,
                                           confirm_text_.c_str());
        if (!queued) { SetFeedback("发送失败（未连接）"); dirty_ = true; return; }
        SetFeedback("发送中：语音指令");
    } else {
        /* No focused agent and no usable defaults (task spec item 4). */
        SetFeedback("无发送目标：无 agent 且未配默认 cwd");
        dirty_ = true;
        return;
    }
    screen_ = SCR_OVERVIEW;
    paseo_client_fetch_agents(); /* spec: refresh the list after sending */
    dirty_ = true;
}

void PaseoUi::DiscardConfirm(const char *why)
{
    screen_ = voice_from_;
    if (why) SetFeedback("%s", why);
    dirty_ = true;
}

/* ---------------- key dispatch ---------------- */

void PaseoUi::HandleKey(int key, int kind)
{
    if (!overlay_) return;
    if (kind == PASEO_KEY_PRESS_DOWN) {
        OnPressDown(key);
        if (dirty_) Render();
        return;
    }
    if (kind == PASEO_KEY_PRESS_UP) {
        OnPressUp(key);
        if (dirty_) Render();
        return;
    }
    /* CLICK / LONG from here on. */
    int64_t now = NowMs();
    bool click = kind == PASEO_KEY_CLICK;

    /* While a voice take is tracked, framework click/long events belong
     * to the tracked press (or are noise): they must not act. */
    if (voice_ui_ != V_NONE) return;

    /* The trailing framework CLICK of a synthesized tap fires late;
     * swallow it (exactly one, inside its time window). */
    if (key == PASEO_KEY_OK && click && ok_swallow_click_) {
        ok_swallow_click_ = false;
        return;
    }
    /* Nav clicks trailing a voice cancel are likewise dead. */
    if (key != PASEO_KEY_OK && now < nav_ignore_until_ms_) return;

    /* OK long-press has no function since phase 3: holding OK is the
     * voice gesture, and every phase-2 OK-long action moved to
     * DOWN long-press. */
    if (key == PASEO_KEY_OK && !click) return;

    switch (screen_) {
    case SCR_OVERVIEW:
        if (key == PASEO_KEY_UP)
            focus_ -= click ? 1 : 5;
        else if (key == PASEO_KEY_DOWN && click)
            focus_ += 1;
        else if (key == PASEO_KEY_DOWN) { /* DOWN long: phase-2 OK long */
            if (perm_count_ > 0) {
                screen_ = SCR_PERMS;
                perm_focus_ = 0;
            } else {
                SetFeedback("当前无待审批");
            }
            break;
        } else if (key == PASEO_KEY_OK) { /* click (untracked/offline) */
            EnterResult();
            break;
        } else
            break;
        if (focus_ < 0) focus_ = 0;
        if (focus_ >= agent_count_)
            focus_ = agent_count_ > 0 ? agent_count_ - 1 : 0;
        break;
    case SCR_RESULT: {
        int max_scroll = (int)wrapped_.size() - kTextRows;
        if (max_scroll < 0) max_scroll = 0;
        if (key == PASEO_KEY_UP)
            scroll_ -= click ? 1 : 10;
        else if (key == PASEO_KEY_DOWN && click)
            scroll_ += 1;
        else if (key == PASEO_KEY_DOWN) { /* DOWN long: back */
            screen_ = SCR_OVERVIEW;
            break;
        } else if (key == PASEO_KEY_OK) {
            EnterMenu();
            break;
        } else
            break;
        if (scroll_ < 0) scroll_ = 0;
        if (scroll_ > max_scroll) scroll_ = max_scroll;
        break;
    }
    case SCR_PERMS:
        if (key == PASEO_KEY_UP)
            perm_focus_ = click ? perm_focus_ - 1 : 0;
        else if (key == PASEO_KEY_DOWN && click)
            perm_focus_ += 1;
        else if (key == PASEO_KEY_DOWN) { /* DOWN long: reject (was OK long) */
            if (perm_focus_ >= perm_count_) {
                screen_ = SCR_OVERVIEW;
            } else {
                ResolvePerm(false);
            }
            break;
        } else if (key == PASEO_KEY_OK && click) {
            if (perm_focus_ >= perm_count_) {
                screen_ = SCR_OVERVIEW;
            } else {
                ResolvePerm(true);
            }
            break;
        } else
            break;
        if (perm_focus_ < 0) perm_focus_ = 0;
        if (perm_focus_ > perm_count_) perm_focus_ = perm_count_;
        break;
    case SCR_MENU:
        if (key == PASEO_KEY_UP)
            menu_focus_ -= click ? 1 : 5;
        else if (key == PASEO_KEY_DOWN && click)
            menu_focus_ += 1;
        else if (key == PASEO_KEY_DOWN) { /* DOWN long: back (was OK long) */
            screen_ = menu_return_;
            break;
        } else if (key == PASEO_KEY_OK && click) {
            ExecMenuItem(menu_focus_);
            break;
        } else
            break;
        if (menu_focus_ < 0) menu_focus_ = 0;
        if (menu_focus_ >= kMenuCount) menu_focus_ = kMenuCount - 1;
        break;
    case SCR_CONFIRM:
        if (key == PASEO_KEY_OK && click) {
            if (now >= confirm_ok_guard_ms_) SendConfirmed();
        } else if (key == PASEO_KEY_UP || key == PASEO_KEY_DOWN) {
            DiscardConfirm("语音指令已作废");
        }
        break;
    }
    dirty_ = true;
    Render();
}

void PaseoUi::Tick()
{
    int64_t now = NowMs();

    /* Voice: promote a tracked press to a recording at the threshold. */
    if (voice_ui_ == V_PENDING && now - ok_down_ms_ >= kVoiceHoldMs) {
        if (!voice_ready_) {
            voice_ui_ = V_ABORT;
            SetFeedback("录音不可用（存储未就绪）");
        } else if (!voice_.HasAsrKey()) {
            voice_ui_ = V_ABORT;
            SetFeedback("未配置 ASR Key（配网页填写）");
        } else if (voice_.StartRecording()) {
            voice_ui_ = V_REC;
            ESP_LOGI(TAG, "voice recording started");
        } else {
            voice_ui_ = V_ABORT;
            SetFeedback("录音启动失败");
        }
        dirty_ = true;
    }
    /* Mirror the engine's own transitions (60 s cap auto-stop moves it
     * to UPLOADING while OK may still be held). */
    if (voice_ui_ == V_REC && voice_.State() == PV_UPLOADING)
        voice_ui_ = V_UPLOAD;

    /* Voice results arrive on the result queue. */
    PaseoVoiceResult res;
    if (voice_.PollResult(res)) {
        if (res.cancelled) {
            if (voice_ui_ == V_REC || voice_ui_ == V_UPLOAD) {
                voice_ui_ = V_NONE;
                SetFeedback("已取消");
            }
            dirty_ = true;
        } else if (voice_ui_ == V_UPLOAD) {
            voice_ui_ = V_NONE;
            if (res.ok) {
                EnterConfirm(res.text);
            } else {
                SetFeedback("%s", res.err);
            }
            dirty_ = true;
        } else if (voice_ui_ == V_REC) {
            /* Recording ended on its own (file error etc.). */
            voice_ui_ = V_NONE;
            if (!res.ok) SetFeedback("%s", res.err);
            dirty_ = true;
        }
        /* Otherwise the take was abandoned on the UI side already
         * (cancel / Hide): drop the result silently. */
    }

    /* Confirm screen idle timeout discards the take. */
    if (screen_ == SCR_CONFIRM && now > confirm_deadline_ms_) {
        DiscardConfirm("语音指令超时作废");
    }

    if (ok_swallow_click_ && now > ok_swallow_until_ms_)
        ok_swallow_click_ = false;

    if (resolving_id_[0] && now - resolving_since_ms_ > 10000) {
        /* No resolved event within 10 s: reconcile against a fresh
         * snapshot instead of trusting local state (subset doc 4.4). */
        resolving_id_[0] = '\0';
        paseo_client_fetch_agents();
        SetFeedback("审批确认超时，已重新对账");
        dirty_ = true;
    }
    if (!feedback_.empty() && now > feedback_until_ms_) {
        feedback_.clear();
        dirty_ = true;
    }
    if ((voice_ui_ == V_REC || voice_ui_ == V_UPLOAD) && overlay_)
        dirty_ = true; /* elapsed/level animation */
    if (dirty_ && overlay_) Render();
}

/* ---------------- rendering ---------------- */

void PaseoUi::RenderOverview(std::string &head, std::string &body)
{
    int total_perms = 0;
    for (int i = 0; i < agent_count_; i++)
        total_perms += agents_[i].pending_permissions;
    head = "Paseo ";
    head += conn_ == PASEO_ST_ONLINE ? "在线" : "离线";
    head += " · " + std::to_string(agent_count_) + " agents";
    if (total_perms > 0) head += " · 待审批 " + std::to_string(total_perms);

    if (agent_count_ == 0) {
        body = "（暂无 agent）\n\n等待 daemon 推送，\n或在结果屏菜单里新建任务。";
        return;
    }
    int start = focus_ - kListRows / 2;
    if (start < 0) start = 0;
    if (start + kListRows > agent_count_)
        start = agent_count_ > kListRows ? agent_count_ - kListRows : 0;
    for (int i = start; i < agent_count_ && i < start + kListRows; i++) {
        const paseo_agent_t &a = agents_[i];
        std::string line = i == focus_ ? "> " : "  ";
        line += "[";
        line += a.status[0] ? a.status : "?";
        line += "] ";
        line += AgentLabel(a);
        if (a.pending_permissions > 0)
            line += " <待审批x" + std::to_string(a.pending_permissions) + ">";
        body += Truncate(line, kLineUnits);
        body += "\n";
    }
}

void PaseoUi::RenderResult(std::string &head, std::string &body)
{
    std::string title;
    for (int i = 0; i < agent_count_; i++)
        if (strcmp(agents_[i].id, result_agent_) == 0)
            title = AgentLabel(agents_[i]);
    if (title.empty()) title = std::string(result_agent_, 0, 8);
    head = "结果 · " + Truncate(title, kLineUnits - 6);

    if (result_loading_) {
        body = "加载中…";
        return;
    }
    if (!result_error_.empty()) {
        body = "获取失败：" + Truncate(result_error_, kLineUnits * 3);
        return;
    }
    if (wrapped_.empty() || (wrapped_.size() == 1 && wrapped_[0].empty())) {
        body = "（暂无文本输出）";
        return;
    }
    int max_scroll = (int)wrapped_.size() - kTextRows;
    if (max_scroll < 0) max_scroll = 0;
    int sc = scroll_ > max_scroll ? max_scroll : scroll_;
    for (int i = sc; i < (int)wrapped_.size() && i < sc + kTextRows; i++) {
        body += wrapped_[i];
        body += "\n";
    }
    if ((int)wrapped_.size() > kTextRows) {
        char tail[32];
        snprintf(tail, sizeof(tail), "— %d/%d 行", sc + 1,
                 (int)wrapped_.size());
        body += tail;
    }
}

void PaseoUi::RenderPerms(std::string &head, std::string &body)
{
    head = "待审批 (" + std::to_string(perm_count_) + ")";
    if (perm_count_ == 0) {
        body = "（无待审批）";
    }
    for (int i = 0; i < perm_count_; i++) {
        const paseo_perm_t &pm = perms_[i];
        std::string atitle;
        for (int j = 0; j < agent_count_; j++)
            if (strcmp(agents_[j].id, pm.agent_id) == 0)
                atitle = AgentLabel(agents_[j]);
        if (atitle.empty()) atitle = std::string(pm.agent_id, 0, 8);
        std::string line = i == perm_focus_ ? "> " : "  ";
        line += pm.name[0] ? pm.name : "?";
        line += " · ";
        line += pm.summary;
        body += Truncate(line, kLineUnits);
        body += "\n";
        std::string sub = "    @" + atitle;
        if (pm.kind[0]) sub += std::string(" (") + pm.kind + ")";
        body += Truncate(sub, kLineUnits);
        body += "\n";
    }
    std::string back = perm_focus_ >= perm_count_ ? "> " : "  ";
    back += "← 返回总览";
    body += back;
}

void PaseoUi::RenderMenu(std::string &head, std::string &body)
{
    std::string title;
    for (int i = 0; i < agent_count_; i++)
        if (strcmp(agents_[i].id, menu_agent_) == 0)
            title = AgentLabel(agents_[i]);
    if (title.empty()) title = std::string(menu_agent_, 0, 8);
    head = "指令 · " + Truncate(title, kLineUnits - 6);
    for (int i = 0; i < kMenuCount; i++) {
        std::string line = i == menu_focus_ ? "> " : "  ";
        line += kMenuItems[i];
        if (i == 4) line += " (" + def_provider_ + ")";
        body += Truncate(line, kLineUnits);
        body += "\n";
    }
}

void PaseoUi::RenderConfirm(std::string &head, std::string &body)
{
    if (confirm_create_) {
        head = "语音 → 新建任务 (" + def_provider_ + ")";
    } else {
        std::string title;
        for (int i = 0; i < agent_count_; i++)
            if (strcmp(agents_[i].id, confirm_agent_) == 0)
                title = AgentLabel(agents_[i]);
        if (title.empty() && confirm_agent_[0])
            title = std::string(confirm_agent_, 0, 8);
        head = "语音 → " + Truncate(title, kLineUnits - 6);
    }
    int rows = (int)confirm_lines_.size();
    for (int i = 0; i < rows && i < kTextRows; i++) {
        body += confirm_lines_[i];
        body += "\n";
    }
    if (rows > kTextRows) body += "…（全文将完整发送）";
    int64_t left = (confirm_deadline_ms_ - NowMs()) / 1000;
    if (left < 0) left = 0;
    body += "\n（" + std::to_string(left) + " 秒内未确认将作废）";
}

void PaseoUi::RenderVoice(std::string &head, std::string &body)
{
    head = "语音指令";
    if (voice_ui_ == V_REC || voice_.State() == PV_RECORDING) {
        int ms = voice_.ElapsedMs();
        char line[64];
        snprintf(line, sizeof(line), "● 录音中 %d.%d 秒 / 60", ms / 1000,
                 (ms % 1000) / 100);
        body = line;
        body += "\n\n电平 ";
        int cells = voice_.Level() / 10;
        if (cells > 10) cells = 10;
        for (int i = 0; i < 10; i++) body += i < cells ? "█" : "░";
        body += "\n\n松开 OK 后自动识别，\n识别文字需确认才发送。";
    } else {
        body = "识别中…\n\n正在上传并转写，\n请稍候。";
    }
}

void PaseoUi::Render()
{
    if (!overlay_) return;
    std::string head, body;
    const char *hint = "";
    if (voice_ui_ == V_REC || voice_ui_ == V_UPLOAD) {
        RenderVoice(head, body);
        hint = voice_ui_ == V_REC ? "松开OK 识别 · 上/下 取消"
                                  : "上/下 取消（结果丢弃）";
    } else {
        switch (screen_) {
        case SCR_OVERVIEW:
            RenderOverview(head, body);
            hint = "OK 结果 · 长按↓ 审批 · 按住OK 说话";
            break;
        case SCR_RESULT:
            RenderResult(head, body);
            hint = "OK 指令 · 长按↓ 返回 · 按住OK 说话";
            break;
        case SCR_PERMS:
            RenderPerms(head, body);
            hint = "OK 批准 · 长按↓ 驳回";
            break;
        case SCR_MENU:
            RenderMenu(head, body);
            hint = "OK 执行 · 长按↓ 返回";
            break;
        case SCR_CONFIRM:
            RenderConfirm(head, body);
            hint = "OK 发送 · 上/下 作废";
            break;
        }
    }
    if (voice_ui_ == V_PENDING) hint = "继续按住说话，松手取消点击";
    std::string foot = hint;
    if (!feedback_.empty()) {
        foot += "\n";
        foot += feedback_;
    }
    DisplayLockGuard lock(display_);
    if (!lock) return;
    lv_label_set_text((lv_obj_t *)head_label_, head.c_str());
    lv_label_set_text((lv_obj_t *)body_label_, body.c_str());
    lv_label_set_text((lv_obj_t *)foot_label_, foot.c_str());
    dirty_ = false;
}

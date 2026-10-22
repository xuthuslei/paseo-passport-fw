#pragma once

/* Phase-2/3 screens for the Paseo Passport (overview / result /
 * permissions / command menu / voice confirm), rendered as one LVGL
 * overlay on top of the stock chat UI. All UI state lives here (never
 * in the xiaozhi Application).
 *
 * Threading: the paseo_client callbacks (OnAgents/OnPerms/OnTimeline/
 * OnAction) run on the client task and only copy data + set a dirty
 * flag. HandleKey()/Tick() run on the paseo_app task and are the only
 * places that touch LVGL. Recording/ASR run on the PaseoVoice task;
 * Tick() polls its results.
 *
 * Key model (phase 3, see README_PHASE3.md for the full table):
 *  - OK short press = primary action of the current screen.
 *  - Holding OK is the voice gesture: on the overview/result screens a
 *    press still held after ~350 ms starts recording, release stops
 *    it; such a press never also fires the screen's OK action, and OK
 *    long-press (2000 ms framework event) has no function any more.
 *  - Every function that lived on OK long-press in phase 2 moved to
 *    DOWN long-press (overview->perms, result->back, perms reject,
 *    menu back). UP long-press keeps its scroll/jump meanings. */
#include <string>
#include <vector>

#include "paseo_client.h"
#include "paseo_voice.h"

class Display;

class PaseoUi {
public:
    void Init(Display *display, const std::string &def_provider,
              const std::string &def_cwd, const std::string &asr_endpoint,
              const std::string &asr_model, const std::string &asr_key);
    void Show(); /* create the overlay (entering the online UI) */
    void Hide(); /* delete the overlay (leaving the online UI) */
    bool Visible() const { return overlay_ != nullptr; }

    /* paseo_client callback sinks (client task context) */
    void OnState(paseo_state_t st);
    void OnAgents(const paseo_agent_t *agents, int count);
    void OnPerms(const paseo_perm_t *perms, int count);
    void OnTimeline(const char *agent_id, const char *text,
                    const char *error);
    void OnAction(paseo_action_t action, bool ok, const char *agent_id,
                  const char *error);

    /* app task context */
    void HandleKey(int key, int kind);
    void Tick(); /* timers + dirty re-render; call ~5x per second */

private:
    enum Screen { SCR_OVERVIEW, SCR_RESULT, SCR_PERMS, SCR_MENU, SCR_CONFIRM };
    /* UI-side view of the voice flow (the engine has its own state). */
    enum VoiceUi {
        V_NONE = 0,  /* idle */
        V_PENDING,   /* OK held, not yet past the voice threshold */
        V_ABORT,     /* press consumed but recording could not start */
        V_REC,       /* recording */
        V_UPLOAD,    /* ASR request in flight */
    };

    void Render();
    void RenderOverview(std::string &head, std::string &body);
    void RenderResult(std::string &head, std::string &body);
    void RenderPerms(std::string &head, std::string &body);
    void RenderMenu(std::string &head, std::string &body);
    void RenderConfirm(std::string &head, std::string &body);
    void RenderVoice(std::string &head, std::string &body);
    void SetFeedback(const char *fmt, ...);
    void EnterResult();
    void EnterMenu();
    void ExecMenuItem(int idx);
    void ResolvePerm(bool allow);
    const paseo_agent_t *FocusedAgent() const;
    void RebuildWrappedText();

    /* phase 3 voice flow */
    void OnPressDown(int key);
    void OnPressUp(int key);
    void SynthOkClick(); /* a sub-threshold OK tap on overview/result */
    void EnterConfirm(const char *text);
    void SendConfirmed();
    void DiscardConfirm(const char *why);

    Display *display_ = nullptr;
    void *overlay_ = nullptr; /* lv_obj_t*, kept opaque in the header */
    void *head_label_ = nullptr;
    void *body_label_ = nullptr;
    void *foot_label_ = nullptr;

    std::string def_provider_ = "codex";
    std::string def_cwd_;

    /* snapshots copied from client callbacks */
    paseo_state_t conn_ = PASEO_ST_OFFLINE;
    paseo_agent_t agents_[PASEO_MAX_AGENTS];
    int agent_count_ = 0;
    paseo_perm_t perms_[PASEO_MAX_PERMS];
    int perm_count_ = 0;
    bool dirty_ = true;

    Screen screen_ = SCR_OVERVIEW;
    int focus_ = 0;      /* overview: focused agent index */
    int perm_focus_ = 0; /* perms: focused row (== perm_count_ -> back row) */
    int menu_focus_ = 0;

    /* result screen */
    char result_agent_[64] = "";
    std::string result_text_;      /* last assistant text (client-capped) */
    std::vector<std::string> wrapped_; /* result_text_ wrapped for display */
    bool result_loading_ = false;
    std::string result_error_;
    int scroll_ = 0;               /* first visible wrapped line */

    /* command menu */
    char menu_agent_[64] = "";
    Screen menu_return_ = SCR_RESULT;

    /* permission resolution tracking (subset doc 4.4 flow) */
    char resolving_id_[64] = "";
    int64_t resolving_since_ms_ = 0;

    /* transient one-line feedback shown in the footer */
    std::string feedback_;
    int64_t feedback_until_ms_ = 0;

    /* ---- phase 3 voice ---- */
    PaseoVoice voice_;
    bool voice_ready_ = false; /* storage mounted + worker task running */
    VoiceUi voice_ui_ = V_NONE;
    Screen voice_from_ = SCR_OVERVIEW; /* screen the press started on */
    int64_t ok_down_ms_ = 0;
    bool ok_swallow_click_ = false; /* drop the framework CLICK of a tap */
    int64_t ok_swallow_until_ms_ = 0;
    int64_t nav_ignore_until_ms_ = 0; /* stray nav keys after a cancel */
    int64_t confirm_ok_guard_ms_ = 0; /* ignore OK just after confirm shows */

    /* voice confirm screen */
    std::string confirm_text_;
    std::vector<std::string> confirm_lines_;
    char confirm_agent_[64] = ""; /* target agent id (create mode: empty) */
    bool confirm_create_ = false;
    int64_t confirm_deadline_ms_ = 0;
};

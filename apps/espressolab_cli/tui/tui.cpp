#include "tui.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ftxui.hpp>

#include "espressolab/artifact_io.hpp"
#include "espressolab/execution.hpp"
#include "tui_forms.hpp"

// FTXUI-specific rendering and event handling only (issues #23, #24, #28).
// Every input field, default, validation rule, and workflow call lives in
// `tui_forms.{hpp,cpp}`, which has no dependency on FTXUI or a terminal and
// is exercised directly by tests/unit/test_tui_forms.cpp. This keeps the
// terminal UI dependency isolated from the rest of the dependency graph
// (CLAUDE.md architecture) and lets navigation/form logic be unit-tested
// without a TTY (issue #24 acceptance criterion).
namespace {

using namespace ftxui;
using espressolab::CancellationCallback;
using espressolab::tui::Command;
using espressolab::tui::commands;
using espressolab::tui::default_fields;
using espressolab::tui::Field;
using espressolab::tui::InputError;
using espressolab::tui::JobFunction;
using espressolab::tui::JobResult;
using espressolab::tui::make_job;

constexpr int kOk = 0;
constexpr int kInputError = 3;

std::string seconds_text(std::chrono::steady_clock::time_point started) {
    return espressolab::tui::format_number(
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

enum class View { menu, form, running, result };

class TuiComponent final : public ComponentBase {
public:
    explicit TuiComponent(App& app) : app_(app) {}

    ~TuiComponent() override {
        cancel_requested_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    Element OnRender() override {
        std::scoped_lock lock(mutex_);
        std::vector<Element> rows;
        rows.push_back(text("ESPRESSOLAB // TERMINAL WORKBENCH") | bold);
        rows.push_back(text("Native numbers. Reproducible artifacts. No browser formulas."));
        rows.push_back(separator());
        if (view_ == View::menu) render_menu(rows);
        if (view_ == View::form) render_form(rows);
        if (view_ == View::running) render_running(rows);
        if (view_ == View::result) render_result(rows);
        rows.push_back(separator());
        rows.push_back(text(help_text()));
        return vbox(std::move(rows)) | border;
    }

    bool OnEvent(Event event) override {
        if (event == Event::Custom) return true;
        std::scoped_lock lock(mutex_);
        if (event == Event::CtrlC) {
            if (view_ == View::running) {
                cancel_requested_.store(true);
                status_ = "cancellation requested";
            } else {
                app_.Exit();
            }
            return true;
        }
        if (view_ == View::menu) return handle_menu(event);
        if (view_ == View::form) return handle_form(event);
        if (view_ == View::running) return handle_running(event);
        return handle_result(event);
    }

private:
    void render_menu(std::vector<Element>& rows) const {
        rows.push_back(text("Choose a workflow") | bold);
        for (std::size_t i = 0; i < commands().size(); ++i) {
            const std::string marker = i == menu_index_ ? "> " : "  ";
            rows.push_back(text(marker + commands()[i].title + "  " + commands()[i].help));
        }
    }

    void render_form(std::vector<Element>& rows) const {
        rows.push_back(text("Configure " + commands()[selected_].title) | bold);
        rows.push_back(text("Enter edits the focused field. Empty values use command defaults."));
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            const std::string marker = i == field_index_ ? (editing_ ? "* " : "> ") : "  ";
            rows.push_back(text(marker + fields_[i].label + ": " + fields_[i].value));
        }
        rows.push_back(text("Enter on a non-editing field starts the job."));
    }

    void render_running(std::vector<Element>& rows) const {
        rows.push_back(text("Running " + commands()[selected_].title) | bold);
        rows.push_back(text(status_));
        if (progress_total_ > 0) {
            rows.push_back(text("progress: " + std::to_string(progress_completed_) + " / " +
                                std::to_string(progress_total_)));
        } else {
            rows.push_back(text("progress: native workflow in progress"));
        }
        rows.push_back(text("elapsed: " + seconds_text(started_) + " s"));
        rows.push_back(text("Press c or Ctrl-C to request cancellation."));
    }

    void render_result(std::vector<Element>& rows) const {
        rows.push_back(text(result_title_) | bold);
        for (const std::string& line : result_lines_) rows.push_back(text(line));
    }

    std::string help_text() const {
        if (view_ == View::menu) return "Up/Down select   Enter open   q exit";
        if (view_ == View::form) return "Up/Down move   Enter edit/run   Tab next   Esc back";
        if (view_ == View::running) return "c cancel   Esc return when complete";
        return "Enter/Esc return to commands   q exit";
    }

    bool handle_menu(Event event) {
        if (event == Event::ArrowUp) {
            if (menu_index_ > 0) --menu_index_;
            return true;
        }
        if (event == Event::ArrowDown) {
            if (menu_index_ + 1 < commands().size()) ++menu_index_;
            return true;
        }
        if (event == Event::Return) {
            selected_ = menu_index_;
            fields_ = default_fields(commands()[selected_].command);
            field_index_ = 0;
            editing_ = false;
            if (fields_.empty()) start_job_locked();
            else view_ = View::form;
            return true;
        }
        if (event == Event::Character("q")) {
            app_.Exit();
            return true;
        }
        return false;
    }

    bool handle_form(Event event) {
        if (event == Event::Escape) {
            if (editing_) editing_ = false;
            else view_ = View::menu;
            return true;
        }
        if (editing_) {
            if (event == Event::Return) {
                editing_ = false;
                return true;
            }
            if (event == Event::Backspace) {
                if (!fields_[field_index_].value.empty()) fields_[field_index_].value.pop_back();
                return true;
            }
            if (event.is_character()) {
                fields_[field_index_].value += event.character();
                return true;
            }
            return false;
        }
        if (event == Event::ArrowUp) {
            if (field_index_ > 0) --field_index_;
            return true;
        }
        if (event == Event::ArrowDown || event == Event::Tab) {
            if (!fields_.empty()) field_index_ = (field_index_ + 1) % fields_.size();
            return true;
        }
        if (event == Event::Return) {
            editing_ = true;
            return true;
        }
        return false;
    }

    bool handle_running(Event event) {
        if (event == Event::Character("c")) {
            cancel_requested_.store(true);
            status_ = "cancellation requested";
            return true;
        }
        return false;
    }

    bool handle_result(Event event) {
        if (event == Event::Return || event == Event::Escape) {
            view_ = View::menu;
            return true;
        }
        if (event == Event::Character("q")) {
            app_.Exit();
            return true;
        }
        return false;
    }

    // Precondition: mutex_ held by the caller (an event handler).
    void start_job_locked() {
        if (worker_.joinable()) worker_.join();
        const Command command = commands()[selected_].command;
        const std::vector<Field> fields = fields_;
        result_title_ = "Result: " + commands()[selected_].title;
        result_lines_.clear();
        progress_completed_ = 0;
        progress_total_ = 0;
        status_ = "starting";
        started_ = std::chrono::steady_clock::now();
        cancel_requested_.store(false);
        view_ = View::running;
        const JobFunction job = make_job(command, fields);
        // Long-running native work runs on this one worker, off the render
        // loop (issue #23: "long-running work runs off the render loop with
        // one active job at a time"). The render/event thread only ever
        // touches `mutex_`-guarded state; the worker posts Event::Custom to
        // wake the render loop after every state change.
        worker_ = std::thread([this, job] {
            JobResult result;
            try {
                result = job([this] { return cancel_requested_.load(); },
                             [this](int completed, int total, std::string status) {
                                 std::scoped_lock lock(mutex_);
                                 progress_completed_ = completed;
                                 progress_total_ = total;
                                 status_ = std::move(status);
                                 app_.PostEvent(Event::Custom);
                             });
            } catch (const espressolab::ExecutionCancelled&) {
                result.cancelled = true;
                result.lines = {"cancelled", "no incomplete single-run artifacts were written"};
            } catch (const InputError& error) {
                result.lines = {"error INVALID_INPUT at " + error.field + ": " + error.what()};
            } catch (const espressolab::artifact_io::LoadError& error) {
                result.lines = {"error " + error.code + " at " + error.path + ": " + error.what()};
            } catch (const espressolab::InvalidInputError& error) {
                for (const auto& issue : error.validation().issues()) {
                    result.lines.push_back("error " + issue.code + " at " + issue.path + ": " + issue.message);
                }
            } catch (const std::exception& error) {
                result.lines = {"error INTERNAL_ERROR: " + std::string(error.what())};
            }
            {
                std::scoped_lock lock(mutex_);
                if (result.cancelled) result_title_ = "Cancelled: " + commands()[selected_].title;
                result_lines_ = std::move(result.lines);
                status_ = result.cancelled ? "cancelled" : "complete";
                view_ = View::result;
            }
            app_.PostEvent(Event::Custom);
        });
    }

    App& app_;
    mutable std::mutex mutex_;
    View view_ = View::menu;
    std::size_t menu_index_ = 0;
    std::size_t selected_ = 0;
    std::vector<Field> fields_;
    std::size_t field_index_ = 0;
    bool editing_ = false;
    std::string status_;
    std::string result_title_;
    std::vector<std::string> result_lines_;
    std::atomic<bool> cancel_requested_{false};
    std::thread worker_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
    int progress_completed_ = 0;
    int progress_total_ = 0;
};

bool has_interactive_terminal() {
#if defined(_WIN32)
    return _isatty(_fileno(stdin)) != 0 && _isatty(_fileno(stdout)) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
#endif
}

}  // namespace

int run_tui() {
    // Reject non-interactive stdin/stdout before entering the render loop
    // (issue #28: "detect interactive stdin/stdout before entering the
    // TUI"). FTXUI's App owns raw-mode/alternate-screen lifecycle and
    // restores terminal state on normal exit, exceptions, resize (SIGWINCH),
    // and Ctrl-C internally (see third_party/ftxui/ftxui.cpp signal
    // handling); nothing below needs to duplicate that.
    if (!has_interactive_terminal()) {
        std::cerr << "error NONINTERACTIVE_TERMINAL: tui requires an interactive POSIX terminal\n";
        return kInputError;
    }
    App app = App::FullscreenAlternateScreen();
    app.TrackMouse(false);
    // FTXUI's default forces every Ctrl-C to kill the process via SIGINT
    // after `OnEvent` returns, regardless of whether the component handled
    // it. That would race a running workflow's cooperative cancellation
    // (issue #31): the process could be killed before the worker thread
    // observes `cancel_requested_` and stops short of writing an artifact.
    // Disabling the force hands Ctrl-C entirely to `TuiComponent::OnEvent`,
    // which requests cancellation while a job is running and exits cleanly
    // (via `App::Exit`, not a signal) everywhere else.
    app.ForceHandleCtrlC(false);
    auto component = std::make_shared<TuiComponent>(app);
    app.Loop(component);
    return kOk;
}

// User-feedback duration policy (Polish Track, Task 4).
//
// One shared home for the four feedback-duration bands. Before this header, every notice call site carried
// its own magic millisecond value, and the same event class (errors, success confirmations, …) was shown for
// five different lengths on five different channels. The constants below name the bands the observed
// durations already clustered into (see docs/superpowers/polish/2026-07-17-jank-inventory.md — the feedback
// inventory + "Proposed feedback POLICY" table). Every feedback call site maps to exactly one class.
//
// Channel is chosen by class, not by call site:
//
//   | Event class                                   | Channel (policy)                       | Constant          | Value |
//   |-----------------------------------------------|----------------------------------------|-------------------|-------|
//   | Error (couldn't do the thing)                 | notifier_->notify (themed) / showToast | kFeedbackLong     | 7000  |
//   | Progress (in-flight: Finding…/Downloading…)   | showToast / statusMessage, sticky      | kFeedbackSticky   | 0     |
//   | Success-confirmation (did it: Added/Removed)  | showToast                              | kFeedbackShort    | 2500  |
//   | Ambient status (a panel/Settings is open)     | statusBar()->showMessage               | kFeedbackStandard | 4500  |
//   | Player-overlay notice (over the video/game)   | notifier_->playerNotice                | kFeedbackLong     | 7000  |
//
// kFeedbackSticky (0) means "no auto-hide": the notice stays until the operation that raised it resolves and
// the code clears it (the result notice replaces it). Never use a raw literal for a feedback duration — reach
// for one of these so the policy stays enforceable in one place.
#pragma once

constexpr int kFeedbackShort    = 2500;  // brief success confirmations (was 2500–3000)
constexpr int kFeedbackStandard = 4500;  // default info/confirmation (already the toast default)
constexpr int kFeedbackLong     = 7000;  // errors that must be read (was 5000–12000)
constexpr int kFeedbackSticky   = 0;     // in-flight progress; cleared when the op resolves

// UI transition/motion length (fades, slides, pushes) — the Polish Track's single shared duration for
// visual transitions (Task 5 rule 1). The themed QML shell reads it through the theme host bridge
// (MainWindow feeds it to the ThemeView root as `uiMotionMs`); this header owns the canonical value.
constexpr int kUiFadeMs = 150;

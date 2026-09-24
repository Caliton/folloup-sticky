#include "onboarding_page_coordinator.h"

#include <array>

#include "asset_manifest.h"
#include "project_assets.h"

namespace {

using page_navigation::NavigationItemRole;
using epaper_ui::OnboardingControl;

struct Slide {
    const char* title;
    const char* body;
    EmbeddedImageId image;
};

constexpr std::array<Slide, 6> kSlides = {{
    {"Boas-vindas ao Followup",
     "Seu caderno de voz de bolso. Registre ideias em voz alta e deixe o Followup organizá-las.",
     EmbeddedImageId::kSlide1},
    // Controls of the Waveshare ESP32-S3-ePaper-3.97: rocker (up/down/press), BOOT, PWR.
    {"Grave e bloqueie",
     "Segure o BOOT para gravar uma nota, ideia ou tarefa e solte para parar. Toque no PWR "
     "para bloquear a tela.",
     EmbeddedImageId::kSlide2},
    {"Navegue pela alavanca",
     "Mova a alavanca para subir ou descer e aperte para selecionar. Segure para baixo para "
     "voltar.",
     EmbeddedImageId::kSlide3},
    {"Repouso e energia",
     "O aparelho repousa sozinho quando parado. Segure o PWR por 1 s para desligar ou 6 s "
     "para forçar.",
     EmbeddedImageId::kSlide4},
    {"Resumos com o Gemini",
     "Conecte o Gemini e deixe o Followup transcrever suas gravações e resumir o seu dia.",
     EmbeddedImageId::kSlide5},
    {"Notas, tarefas e mais",
     "Tudo fica agrupado por dia. Veja em Notas, marque tarefas em Tarefas e fixe o que quiser em "
     "Acompanhar.",
     EmbeddedImageId::kSlide6},
}};

NavigationItemRole RoleForControl(OnboardingControl control)
{
    switch (control) {
        case OnboardingControl::kClose:
            return NavigationItemRole::kOnboardingPageClose;
        case OnboardingControl::kPrev:
            return NavigationItemRole::kOnboardingPagePrev;
        case OnboardingControl::kNext:
            return NavigationItemRole::kOnboardingPageNext;
        case OnboardingControl::kNone:
        default:
            return NavigationItemRole::kUnknown;
    }
}

}  // namespace

OnboardingPageCoordinator::OnboardingPageCoordinator() = default;

int OnboardingPageCoordinator::slide_count() const
{
    return static_cast<int>(kSlides.size());
}

void OnboardingPageCoordinator::PrepareForShow()
{
    active_index_ = 0;
    focus_.Configure(navigation_model_.item_count, 0);
    FocusFirstSelectable();
}

epaper_ui::OnboardingControl OnboardingPageCoordinator::FocusedControl() const
{
    if (IsRoleFocused(NavigationItemRole::kOnboardingPageClose)) {
        return OnboardingControl::kClose;
    }
    if (IsRoleFocused(NavigationItemRole::kOnboardingPagePrev)) {
        return OnboardingControl::kPrev;
    }
    if (IsRoleFocused(NavigationItemRole::kOnboardingPageNext)) {
        return OnboardingControl::kNext;
    }
    return OnboardingControl::kNone;
}

bool OnboardingPageCoordinator::ControlSelectable(OnboardingControl control) const
{
    switch (control) {
        case OnboardingControl::kPrev:
            return !PrevDisabled();
        case OnboardingControl::kNext:
            return !NextDisabled();
        case OnboardingControl::kClose:
            return ShowClose();  // only on the last slide
        case OnboardingControl::kNone:
        default:
            return false;
    }
}

void OnboardingPageCoordinator::FocusRole(NavigationItemRole role)
{
    const int index = navigation_model_.IndexOfRole(role);
    if (index >= 0) {
        focus_.SetIndex(index);
    }
}

void OnboardingPageCoordinator::FocusFirstSelectable()
{
    // Nav order is [Close, Prev, Next]; pick the first control that can currently be focused.
    for (const OnboardingControl control :
         {OnboardingControl::kClose, OnboardingControl::kPrev, OnboardingControl::kNext}) {
        if (ControlSelectable(control)) {
            FocusRole(RoleForControl(control));
            return;
        }
    }
}

bool OnboardingPageCoordinator::IsRoleFocused(NavigationItemRole role) const
{
    return navigation_model_.IsRoleSelected(focus_.index(), role);
}

bool OnboardingPageCoordinator::MoveFocus(int delta)
{
    if (delta == 0) {
        return false;
    }
    // UP/DOWN pages the carousel rather than roving the control row.
    //
    // Roving does not work here: the row is Close/Prev/Next, and on the first slide -- where
    // onboarding always opens -- Prev is disabled and Close is hidden, leaving Next as the
    // only selectable control. The old loop then wrapped a full circle back onto Next and
    // still reported success, so UP/DOWN played a cue and repainted without ever moving.
    // Paging is also what the keys mean everywhere else in the app, and it keeps every slide
    // reachable without touch. EnsureFocusEnabled (inside Next/PrevSlide) keeps the focused
    // control valid, so a click still activates Next, or Close on the last slide.
    return delta > 0 ? NextSlide() : PrevSlide();
}

bool OnboardingPageCoordinator::SetFocusIndex(int index)
{
    return focus_.SetIndex(index);
}

void OnboardingPageCoordinator::EnsureFocusEnabled()
{
    const OnboardingControl focused = FocusedControl();
    if (ControlSelectable(focused)) {
        return;
    }
    // Reaching the last slide: Next disables and Close appears -> land on Close (the primary CTA).
    if (focused == OnboardingControl::kNext && ShowClose()) {
        FocusRole(NavigationItemRole::kOnboardingPageClose);
        return;
    }
    // Reaching the first slide: Prev disables -> land on Next.
    if (focused == OnboardingControl::kPrev && !NextDisabled()) {
        FocusRole(NavigationItemRole::kOnboardingPageNext);
        return;
    }
    FocusFirstSelectable();
}

bool OnboardingPageCoordinator::NextSlide()
{
    if (NextDisabled()) {
        return false;
    }
    ++active_index_;
    EnsureFocusEnabled();
    return true;
}

bool OnboardingPageCoordinator::PrevSlide()
{
    if (PrevDisabled()) {
        return false;
    }
    --active_index_;
    EnsureFocusEnabled();
    return true;
}

bool OnboardingPageCoordinator::FocusControl(OnboardingControl control)
{
    const NavigationItemRole role = RoleForControl(control);
    if (role == NavigationItemRole::kUnknown || !ControlSelectable(control)) {
        return false;
    }
    const int index = navigation_model_.IndexOfRole(role);
    if (index < 0) {
        return false;
    }
    return focus_.SetIndex(index);
}

epaper_ui::OnboardingPageState OnboardingPageCoordinator::BuildState() const
{
    epaper_ui::OnboardingPageState state = {};
    state.navigation_focus_index = focus_.index();
    state.carousel.slide_count = slide_count();
    state.carousel.active_index = active_index_;
    state.carousel.show_close = ShowClose();
    state.carousel.close_selected = IsRoleFocused(NavigationItemRole::kOnboardingPageClose);
    state.carousel.prev_selected = IsRoleFocused(NavigationItemRole::kOnboardingPagePrev);
    state.carousel.next_selected = IsRoleFocused(NavigationItemRole::kOnboardingPageNext);

    const Slide& slide = kSlides[static_cast<size_t>(active_index_)];
    state.slide_title = slide.title;
    state.slide_body = slide.body;
    state.slide_image = project_assets::GetImage(slide.image);
    return state;
}

// Host-side screen preview: compiles the real epaper_ui drawing code for the PC and dumps
// every scene below as a portrait 1-bit PBM. scripts/preview_screens.py builds this file and
// converts the output to PNG. Scene data mirrors what the main/ coordinators produce on-device.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "epaper_ui/books_page.h"
#include "epaper_ui/card_modal.h"
#include "epaper_ui/dashboard_page.h"
#include "epaper_ui/details_page.h"
#include "epaper_ui/journal_page.h"
#include "epaper_ui/global_footer.h"
#include "epaper_ui/keyboard.h"
#include "epaper_ui/lock_screen.h"
#include "epaper_ui/notes_page.h"
#include "epaper_ui/onboarding_page.h"
#include "epaper_ui/reader_page.h"
#include "epaper_ui/select_modal.h"
#include "epaper_ui/settings_page.h"
#include "epaper_ui/status_bar.h"
#include "epaper_ui/sticky_note.h"
#include "epaper_ui/text_layout.h"
#include "epaper_ui/summarize_page.h"
#include "epaper_ui/time_page.h"
#include "epaper_ui/toast.h"
#include "epaper_ui/vibe_check_page.h"
#include "epaper_ui/welcome_message.h"
#include "epaper_ui/wifi_page.h"
#include "journal_view.h"
#include "project_assets.h"

namespace {

// Same geometry as components/board/include/waveshare_board_config.h: the panel is 800x480
// landscape, the UI draws in portrait (480x800) and rotates into the raw buffer.
constexpr int kRawWidth = 800;
constexpr int kRawHeight = 480;
constexpr int kPortraitWidth = kRawHeight;
constexpr int kPortraitHeight = kRawWidth;
constexpr size_t kBufferLen = (kRawWidth * kRawHeight) / 8;

using Framebuffer = std::vector<uint8_t>;

// ---------------------------------------------------------------------------------------------
// Shared chrome
// ---------------------------------------------------------------------------------------------

epaper_ui::StatusBarState StatusBar()
{
    epaper_ui::StatusBarState state = {};
    state.battery = {.percent = 78, .charging = false};
    state.wifi = epaper_ui::WifiStatus::kConnected;
    state.time_text = "14:32";
    state.show_gemini_icon = true;
    return state;
}

// Mirrors app_shell's default footer layout (every button visible).
epaper_ui::GlobalFooterState Footer(epaper_ui::GlobalFooterItemId selected =
                                        epaper_ui::GlobalFooterItemId::kNone)
{
    using epaper_ui::GlobalFooterItemId;
    auto button = [&](GlobalFooterItemId id, EmbeddedIconId icon) {
        return epaper_ui::FooterButtonState{
            .visible = true,
            .selected = selected == id,
            .icon = project_assets::GetIcon(icon),
        };
    };

    epaper_ui::GlobalFooterState state = {};
    state.visible = true;
    state.home = button(GlobalFooterItemId::kHome, EmbeddedIconId::kHome);
    state.settings = button(GlobalFooterItemId::kSettings, EmbeddedIconId::kSettings);
    state.today = button(GlobalFooterItemId::kToday, EmbeddedIconId::kTaskStart);
    state.sticky = button(GlobalFooterItemId::kSticky, EmbeddedIconId::kSticky);
    state.mic.visible = true;
    state.mic.selected = selected == GlobalFooterItemId::kMic;
    state.mic.idle_icon = project_assets::GetIcon(EmbeddedIconId::kMicOff);
    state.mic.active_icon = project_assets::GetIcon(EmbeddedIconId::kMicOn);
    return state;
}

epaper_ui::ListItemState Item(const char* time,
                              const char* duration,
                              const char* tag,
                              const char* body,
                              bool pinned = false,
                              bool transcribed = true)
{
    epaper_ui::ListItemState item = {};
    item.header.icon_asset = project_assets::GetIcon(transcribed ? EmbeddedIconId::kTranscribe
                                                                 : EmbeddedIconId::kAudio);
    item.header.tag_icon_asset = pinned ? project_assets::GetIcon(EmbeddedIconId::kPin) : nullptr;
    item.header.time_text = time;
    item.header.minute_seconds_text = duration;
    item.header.tag_text = tag;
    item.body_text = body;
    return item;
}

epaper_ui::TimelineListState NotesTimeline()
{
    epaper_ui::TimelineListState timeline = {};
    timeline.item_label_singular = "nota";
    timeline.item_label_plural = "notas";
    timeline.empty_state_text = "Nenhuma nota ainda";
    timeline.empty_state_icon_asset = project_assets::GetIcon(EmbeddedIconId::kIdea);
    timeline.visible_group_index = 0;
    timeline.groups = {
        {"Hoje",
         {Item("14:05", "42s", "Ideia",
               "Fazer um suporte de mesa pro aparelho usando a mesma dobradiça do case.", true),
          Item("11:20", "1min", "Nota",
               "Reunião com o time: revisar o fluxo de onboarding e as mensagens de erro do Wi-Fi."),
          Item("09:02", "8s", "Nota", "Nota só em áudio.", false, false)}},
        {"Ontem",
         {Item("18:40", "25s", "Ideia", "Testar modo escuro invertido no leitor de livros.")}},
    };
    return timeline;
}

// ---------------------------------------------------------------------------------------------
// Pages. Each scene draws exactly like display_service: clear to white, draw the page (which
// draws its own status bar/footer), then any overlays on top.
// ---------------------------------------------------------------------------------------------

epaper_ui::DashboardPageState Dashboard()
{
    epaper_ui::DashboardPageState state = {};
    state.welcome_message.current_date = {.weekday_text = "Sábado",
                                          .date_text = "26 de setembro de 2026"};
    state.welcome_message.title_text = epaper_ui::WelcomeMessageTitle(0);
    state.current_progress = {.label_text = "Tarefas de hoje",
                              .status_text = "1/3 concluída",
                              .progress_percent = 33};
    return state;
}

void DrawDashboard(uint8_t* fb, const epaper_ui::DashboardPageState& state)
{
    epaper_ui::DrawDashboardPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight,
                                 state, StatusBar(), Footer());
}

void SceneDashboard(uint8_t* fb) { DrawDashboard(fb, Dashboard()); }

void SceneDashboardFocus(uint8_t* fb)
{
    epaper_ui::DashboardPageState state = Dashboard();
    state.menu.selected_index = static_cast<int>(epaper_ui::DashboardMenuItem::kIdeas);
    state.menu.shows_notes_badge = true;
    DrawDashboard(fb, state);
}

void SceneDashboardEmpty(uint8_t* fb)
{
    epaper_ui::DashboardPageState state = Dashboard();
    state.current_progress = {};
    state.shows_completion_banner = true;
    state.completion_banner = {.icon = EmbeddedIconId::kTaskStart,
                               .message_text = "Grave sua primeira nota com o microfone"};
    DrawDashboard(fb, state);
}

void SceneLockScreen(uint8_t* fb)
{
    epaper_ui::LockScreenState state = {};
    state.hour_text = "14";
    state.minute_text = "32";
    state.weekday_text = "Sábado";
    state.date_text = "26 de setembro de 2026";
    epaper_ui::DrawLockScreen(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                              StatusBar());
}

struct Slide {
    const char* title;
    const char* body;
    EmbeddedImageId image;
};

// Copy of kSlides in main/onboarding_page_coordinator.cpp.
constexpr Slide kSlides[] = {
    {"Boas-vindas ao Followup",
     "Seu caderno de voz de bolso. Registre ideias em voz alta e deixe o Followup organizá-las.",
     EmbeddedImageId::kSlide1},
    {"Grave e bloqueie",
     "Segure o BOOT para gravar uma nota, ideia ou tarefa e solte para parar. Toque duas "
     "vezes no PWR para bloquear.",
     EmbeddedImageId::kSlide2},
    {"Navegue pela alavanca",
     "Mova a alavanca para subir ou descer e aperte para selecionar. Toque no PWR para "
     "voltar.",
     EmbeddedImageId::kSlide3},
    {"Repouso e energia",
     "O aparelho repousa sozinho quando parado. Segure o PWR por 1 s para desligar ou 6 s "
     "para forçar.",
     EmbeddedImageId::kSlide4},
    {"Resumos com o Gemini",
     "Conecte o Gemini e deixe o Followup transcrever suas gravações e resumir o seu dia.",
     EmbeddedImageId::kSlide5},
    {"Diário, ideias e mais",
     "Planeje no Diário por ano, mês, semana e dia. Ideias ficam em Ideias. O Sticky mostra o seu "
     "dia na tela.",
     EmbeddedImageId::kSlide6},
};
constexpr int kSlideCount = static_cast<int>(sizeof(kSlides) / sizeof(kSlides[0]));

void SceneOnboarding(uint8_t* fb, int index)
{
    epaper_ui::OnboardingPageState state = {};
    state.carousel.slide_count = kSlideCount;
    state.carousel.active_index = index;
    state.carousel.show_close = index == kSlideCount - 1;
    state.carousel.next_selected = index < kSlideCount - 1;
    state.carousel.close_selected = index == kSlideCount - 1;
    state.slide_title = kSlides[index].title;
    state.slide_body = kSlides[index].body;
    state.slide_image = project_assets::GetImage(kSlides[index].image);
    epaper_ui::DrawOnboardingPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight,
                                  state, StatusBar());
}

void SceneSettings(uint8_t* fb)
{
    epaper_ui::SettingsPageState state = {};
    state.wifi_toggle = {.label_text = "Wi-Fi",
                         .toggle_state = epaper_ui::ToggleVisualState::kFocusOn};
    state.access_point_toggle = {.label_text = "Ponto de acesso",
                                 .toggle_state = epaper_ui::ToggleVisualState::kOff};
    state.sound_toggle = {.label_text = "Sons", .toggle_state = epaper_ui::ToggleVisualState::kOn};
    state.storage_status = {.has_sd_card = true, .free_space_text = "12,3 GB", .used_percent = 34};
    state.enable_otg_button = {.label_text = "Ativar OTG"};
    state.format_sd_button = {.label_text = "Formatar SD"};
    state.manual_onboarding_button = {.label_text = "Manual"};
    state.wifi_page_button = {.label_text = "Wi-Fi"};
    state.time_page_button = {.label_text = "Data e hora"};
    state.app_link_button = {.label_text = "Conectar app"};
    epaper_ui::DrawSettingsPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                                StatusBar(), Footer(epaper_ui::GlobalFooterItemId::kSettings));
}

epaper_ui::WifiPageState Wifi()
{
    epaper_ui::WifiPageState state = {};
    state.network_list.status = epaper_ui::NetworkListStatus::kNetworksFound;
    state.network_list.networks = {
        {.ssid_text = "Casa_5G", .current_network = true, .private_network = true},
        {.ssid_text = "Casa_2.4G", .selected = true, .private_network = true},
        {.ssid_text = "Vizinho",
         .private_network = true,
         .signal_strength = epaper_ui::NetworkSignalStrength::kMedium},
        {.ssid_text = "Café Wi-Fi Grátis",
         .signal_strength = epaper_ui::NetworkSignalStrength::kMedium},
    };
    state.network_list.selected_network_index = 1;
    state.password_input.label_text = "Senha";
    state.password_input.placeholder_text = "Digite a senha";
    state.password_input.submit_style = epaper_ui::KeyboardInputSubmitStyle::kJoin;
    state.scan_button = {.label_text = "Buscar"};
    state.connect_button = {.label_text = "Conectar"};
    return state;
}

void SceneWifi(uint8_t* fb)
{
    epaper_ui::DrawWifiPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, Wifi(),
                            StatusBar(), Footer());
}

void SceneWifiKeyboard(uint8_t* fb)
{
    epaper_ui::WifiPageState page = Wifi();
    page.password_input.value_text = "minhasenha";
    page.password_input.active = true;
    epaper_ui::DrawWifiPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, page,
                            StatusBar(), Footer());

    epaper_ui::KeyboardState keyboard = {};
    keyboard.visible = true;
    keyboard.title_text = "Senha do Wi-Fi";
    keyboard.input = epaper_ui::PasswordInputToKeyboardInput(page.password_input);
    keyboard.selected_key_index = 4;
    epaper_ui::DrawKeyboard(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, keyboard,
                            {});
}

void SceneTime(uint8_t* fb)
{
    epaper_ui::TimePageState state = {};
    state.timezone = {.label_text = "Fuso horário",
                      .placeholder_text = "Escolha o fuso",
                      .value_text = "América/São Paulo (UTC-3)"};
    state.hour = {.value_text = "02", .suffix_text = "H", .focused = true, .max_length = 2};
    state.minute = {.value_text = "32", .suffix_text = "MIN", .max_length = 2};
    state.meridiem = {.label_text = "PM"};
    state.month = {.value_text = "09", .placeholder_text = "MM", .max_length = 2};
    state.day = {.value_text = "26", .placeholder_text = "DD", .max_length = 2};
    state.year = {.value_text = "2026", .placeholder_text = "AAAA", .max_length = 4};
    state.save = {.label_text = "Sincronizar e salvar"};
    epaper_ui::DrawTimePage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                            StatusBar(), Footer());
}

void SceneTimezoneModal(uint8_t* fb)
{
    SceneTime(fb);
    epaper_ui::SelectModalState modal = {};
    modal.visible = true;
    modal.title_text = "Escolha o fuso";
    modal.selected_index = 1;
    modal.items = {{"América/Manaus (UTC-4)"},
                   {"América/São Paulo (UTC-3)"},
                   {"América/Noronha (UTC-2)"},
                   {"Europa/Lisboa (UTC+1)"}};
    epaper_ui::DrawSelectModal(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, modal);
}

epaper_ui::VibeCheckPageState VibeCheck()
{
    epaper_ui::VibeCheckPageState state = {};
    state.card.tag_text = "Sáb, 26 set";
    state.card.header = Item("14:05", "42s", "Ideia", "").header;
    state.card.body_text =
        "Fazer um suporte de mesa pro aparelho usando a mesma dobradiça do case.";
    state.card.empty_state_icon_asset = project_assets::GetIcon(EmbeddedIconId::kIdea);
    state.card.empty_state_message = "Bora começar! Grave algumas ideias!";
    state.progress = {.label_text = "Suas ideias", .status_text = "2/5", .progress_percent = 40};
    return state;
}

void DrawVibeCheck(uint8_t* fb, const epaper_ui::VibeCheckPageState& state)
{
    epaper_ui::DrawVibeCheckPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight,
                                 state, StatusBar(), Footer());
}

void SceneVibeCheck(uint8_t* fb) { DrawVibeCheck(fb, VibeCheck()); }

void SceneVibeCheckEmpty(uint8_t* fb)
{
    epaper_ui::VibeCheckPageState state = VibeCheck();
    state.card.empty = true;
    state.progress = {.label_text = "Suas ideias", .status_text = "0/0"};
    DrawVibeCheck(fb, state);
}

// empty_message: "" shows a summary; otherwise one of the real messages from
// main/summarize_page_coordinator.cpp.
void SceneSummarize(uint8_t* fb, const char* empty_message)
{
    const bool empty = empty_message[0] != '\0';
    epaper_ui::SummarizePageState state = {};
    state.segment_control.labels = {"Ideias", "Semana", ""};
    state.segment_control.segment_count = epaper_ui::kSegmentControlDefaultSegmentCount;
    if (empty) {
        state.scroll_container.empty_state_message = empty_message;
    } else {
        state.scroll_container.content_text =
            "Hoje você registrou 3 notas.\n\n"
            "- Ideia de um suporte de mesa usando a dobradiça do case.\n"
            "- Reunião: revisar o onboarding e as mensagens de erro do Wi-Fi.\n"
            "- Uma nota só em áudio, sem transcrição.";
    }
    state.get_summary_button = {.label_text = "Gerar resumo", .selected = empty};
    epaper_ui::DrawSummarizePage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight,
                                 state, StatusBar(), Footer());
}

epaper_ui::NotesPageState IdeasPage()
{
    epaper_ui::NotesPageState state = {};
    state.timeline = NotesTimeline();
    state.vibe_check_button = {.label_text = "Checar vibe"};
    state.summarize_button = {.label_text = "Resumir"};
    return state;
}

void SceneNotes(uint8_t* fb, bool empty)
{
    epaper_ui::NotesPageState state = IdeasPage();
    if (empty) {
        state.timeline.groups = {{"Hoje", {}}};
    }
    epaper_ui::DrawNotesPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                             StatusBar(), Footer());
}

void SceneNotesSelected(uint8_t* fb)
{
    epaper_ui::NotesPageState state = IdeasPage();
    state.timeline.focused_group_index = 0;
    state.timeline.active_group_index = 0;
    state.timeline.selected_item_index = 1;
    epaper_ui::DrawNotesPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                             StatusBar(), Footer());
}

void SceneDetails(uint8_t* fb)
{
    epaper_ui::DetailsPageState state = {};
    state.recording_header = Item("11:20", "1min", "Nota", "", true).header;
    state.scroll_container.content_text =
        "Reunião com o time: revisar o fluxo de onboarding e as mensagens de erro do Wi-Fi. "
        "Também combinamos de testar a autonomia da bateria com o Wi-Fi desligado e medir "
        "quanto tempo o aparelho aguenta em repouso.";
    state.back_button = {.label_text = "Voltar", .selected = true};
    state.show_transcribe_button = true;
    state.transcribe_button = {.label_text = "Ouvir"};
    epaper_ui::DrawDetailsPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                               StatusBar(), Footer());
}

void SceneBooks(uint8_t* fb, bool empty)
{
    epaper_ui::BooksPageState state = {};
    if (empty) {
        state.message_text = "Nenhum livro. Copie arquivos .epub para a pasta books do cartão SD.";
    } else {
        state.rows = {
            {.title = "Dom Casmurro", .author = "Machado de Assis", .progress_text = "34% lido",
             .selected = true},
            {.title = "O Cortiço", .author = "Aluísio Azevedo", .progress_text = "Novo"},
            {.title = "Memórias Póstumas de Brás Cubas",
             .author = "Machado de Assis",
             .progress_text = "Concluído"},
        };
        state.total_books = 3;
    }
    epaper_ui::DrawBooksPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                             StatusBar(), Footer());
}

// Same line assembly as BuildStateLocked in main/reader_page_runtime.cpp: the runtime wraps
// each block to the text area and hands the page over as '\n'-separated lines.
std::string ReaderPageText(int font_level)
{
    struct Block {
        bool heading;
        const char* text;
    };
    constexpr Block kBlocks[] = {
        {true, "Capítulo II - Do título"},
        {false,
         "Agora que expliquei o título, passo a escrever o livro. Antes disso, porém, digamos "
         "os motivos que me põem a pena na mão."},
        {false,
         "Vivo só, com um criado. A casa em que moro é própria; fi-la construir de propósito, "
         "levado de um desejo tão particular que me vexa imprimi-lo, mas vá lá."},
    };
    const epaper_ui::UiRect area = epaper_ui::ReaderTextArea(kPortraitWidth, kPortraitHeight);
    std::string text;
    for (const Block& block : kBlocks) {
        const std::string_view paragraph = block.text;
        bool first = true;
        epaper_ui::WrapParagraph(
            block.heading ? epaper_ui::ReaderHeadingRole(font_level)
                          : epaper_ui::ReaderBodyRole(font_level),
            paragraph, area.width, [&](const epaper_ui::LineSpan& span) {
                if (!text.empty()) {
                    text.push_back('\n');
                    if (first) {
                        text.push_back('\n');
                    }
                }
                if (block.heading) {
                    text.push_back(epaper_ui::kReaderHeadingMarker);
                }
                text.append(paragraph.substr(span.offset, span.length));
                first = false;
                return true;
            });
    }
    return text;
}

void SceneReader(uint8_t* fb)
{
    epaper_ui::ReaderPageState state = {};
    state.page_text = ReaderPageText(state.font_level);
    state.footer_text = "Cap. 2/12  ·  Pág. 3/18  ·  34%";
    epaper_ui::DrawReaderPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, state,
                              StatusBar());
}

// ---------------------------------------------------------------------------------------------
// Overlays (drawn on top of the dashboard, as display_service does with the underlay)
// ---------------------------------------------------------------------------------------------

void SceneToast(uint8_t* fb)
{
    SceneDashboard(fb);
    epaper_ui::ToastState toast = {};
    toast.visible = true;
    toast.body_text = "Nota salva e transcrita.";
    toast.leading_icon = project_assets::GetIcon(EmbeddedIconId::kCheck);
    toast.show_close_button = true;
    epaper_ui::DrawToast(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, toast);
}

void SceneCardModal(uint8_t* fb)
{
    SceneDashboard(fb);
    epaper_ui::CardModalState modal = {};
    modal.visible = true;
    modal.title_text = "Desligar o aparelho?";
    modal.body_text = "O aparelho será desligado. Deseja continuar?";
    modal.action_labels = {"Cancelar", "Desligar"};
    modal.selected_action_index = 1;
    epaper_ui::DrawCardModal(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, modal);
}

void SceneStickyNote(uint8_t* fb)
{
    SceneDashboard(fb);
    epaper_ui::StickyNoteState sticky = {};
    sticky.visible = true;
    sticky.active_index = 0;
    sticky.sticky_count = 2;
    sticky.date_text = "Hoje";
    sticky.header = Item("14:05", "42s", "Ideia", "", true).header;
    sticky.body_text = "Fazer um suporte de mesa pro aparelho usando a mesma dobradiça do case.";
    sticky.selected_control = epaper_ui::StickyNoteControl::kNext;
    epaper_ui::DrawStickyNote(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, sticky,
                              {});
}

// ---------------------------------------------------------------------------------------------


// ---------------------------------------------------------------------------------------------
// Diário (bullet journal). Built through the real journal_view model, so these scenes are
// exactly what journal_page_coordinator hands to the display. "Today" is Saturday 26/09/2026.
// ---------------------------------------------------------------------------------------------

const journal_period::Date kJournalToday = {2026, 9, 26};

std::vector<journal_view::ViewItem> JournalSampleItems()
{
    using journal_service::ItemStatus;
    using journal_service::ItemType;
    auto item = [](const char* id, ItemType type, const char* text, const char* period,
                   const char* planned, ItemStatus status = ItemStatus::kOpen,
                   bool recording = false) {
        static int64_t created = 1000;
        return journal_view::ViewItem{id, type, status, text, period, planned, recording, ++created};
    };
    return {
        item("p1", ItemType::kTask, "Ligar pro fornecedor da tela.", "2026-09-24", "2026-09-24"),
        item("p2", ItemType::kTask, "Revisar o orçamento do case.", "2026-W38", "2026-09"),
        item("d1", ItemType::kTask, "Comprar bateria 18650 extra.", "2026-09-26", "2026-09-26",
             ItemStatus::kDone, true),
        item("d2", ItemType::kTask, "Mandar o STL do case novo pro grupo.", "2026-09-26",
             "2026-09"),
        item("d3", ItemType::kEvent, "Aniversário da Ana às 19h.", "2026-09-26", "2026-09-26"),
        item("d4", ItemType::kNote, "Testar o modo escuro invertido no leitor.", "2026-09-26",
             "2026-09-26", ItemStatus::kOpen, true),
        item("w1", ItemType::kTask, "Imprimir a moldura nova.", "2026-W39", "2026-09"),
        item("w2", ItemType::kTask, "Organizar a bancada.", "2026-W39", "2026-W39"),
        item("w3", ItemType::kEvent, "Feira de eletrônica no centro.", "2026-09-27", "2026-09-27"),
        item("m1", ItemType::kTask, "Pagar a fatura do cartão.", "2026-09", "2026-09"),
        item("m2", ItemType::kTask, "Renovar a CNH.", "2026-09", "2026-09"),
        item("m3", ItemType::kEvent, "Consulta no dentista às 14h.", "2026-09-30", "2026-09-30"),
        item("o1", ItemType::kTask, "Declarar o carnê-leão.", "2026-10", "2026-10"),
        item("o2", ItemType::kEvent, "Viagem pra Floripa.", "2026-10", "2026-10"),
        item("z1", ItemType::kEvent, "Férias coletivas.", "2026-12", "2026-12"),
        item("y1", ItemType::kTask, "Aprender a soldar SMD.", "2026", "2026"),
        item("y2", ItemType::kNote, "Ler um livro por mês.", "2026", "2026", ItemStatus::kCancelled),
    };
}

void DrawJournal(uint8_t* fb, journal_view::Level level, journal_view::FocusState focus)
{
    const journal_view::Model model =
        journal_view::Build(JournalSampleItems(), level, kJournalToday);
    if (focus.visible_group < 0) {
        focus.visible_group = 0;
    }
    epaper_ui::DrawJournalPage(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight,
                               journal_view::ToPageState(model, level, focus), StatusBar(),
                               Footer());
}

void SceneJournal(uint8_t* fb, journal_view::Level level)
{
    journal_view::FocusState focus = {};
    focus.segment_focused = true;
    DrawJournal(fb, level, focus);
}

void SceneJournalReviewSelected(uint8_t* fb)
{
    journal_view::FocusState focus = {};
    focus.visible_group = 0;
    focus.focused_group = 0;
    focus.active_group = 0;
    focus.selected_item = 0;
    DrawJournal(fb, journal_view::Level::kDay, focus);
}

void SceneJournalTodayActive(uint8_t* fb)
{
    journal_view::FocusState focus = {};
    focus.visible_group = 1;
    focus.focused_group = 1;
    focus.active_group = 1;
    focus.selected_item = 1;
    DrawJournal(fb, journal_view::Level::kDay, focus);
}

void SceneJournalActions(uint8_t* fb)
{
    SceneJournalReviewSelected(fb);
    const std::vector<journal_view::ViewItem> items = JournalSampleItems();
    epaper_ui::SelectModalState modal = {};
    modal.visible = true;
    modal.title_text = "Tarefa";
    for (const journal_view::ActionOption& option :
         journal_view::BuildActions(items.front(), kJournalToday)) {
        modal.items.push_back({option.label});
    }
    epaper_ui::DrawSelectModal(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, modal);
}

void SceneAppLinkCode(uint8_t* fb)
{
    SceneSettings(fb);
    epaper_ui::SelectModalState modal = {};
    modal.visible = true;
    modal.title_text = "No app, digite: K7P 2QX";
    modal.items = {{"Fechar"}, {"Cancelar pareamento"}};
    epaper_ui::DrawSelectModal(fb, kRawWidth, kRawHeight, kPortraitWidth, kPortraitHeight, modal);
}

struct Scene {
    std::string name;
    std::function<void(uint8_t*)> draw;
};

std::vector<Scene> Scenes()
{
    std::vector<Scene> scenes = {
        {"dashboard", SceneDashboard},
        {"dashboard_foco", SceneDashboardFocus},
        {"dashboard_vazio", SceneDashboardEmpty},
        {"bloqueio", SceneLockScreen},
        {"configuracoes", SceneSettings},
        {"configuracoes_codigo_app", SceneAppLinkCode},
        {"wifi", SceneWifi},
        {"wifi_teclado", SceneWifiKeyboard},
        {"data_hora", SceneTime},
        {"data_hora_fuso", SceneTimezoneModal},
        {"checar_vibe", SceneVibeCheck},
        {"checar_vibe_vazio", SceneVibeCheckEmpty},
        {"resumir", [](uint8_t* fb) { SceneSummarize(fb, ""); }},
        {"resumir_vazio", [](uint8_t* fb) { SceneSummarize(fb, "Resuma suas ideias"); }},
        {"resumir_sem_gemini",
         [](uint8_t* fb) { SceneSummarize(fb, "Conecte o Gemini para gerar resumos"); }},
        {"notas", [](uint8_t* fb) { SceneNotes(fb, false); }},
        {"notas_selecionada", SceneNotesSelected},
        {"notas_vazio", [](uint8_t* fb) { SceneNotes(fb, true); }},
        {"diario_dia", [](uint8_t* fb) { SceneJournal(fb, journal_view::Level::kDay); }},
        {"diario_dia_pendente", SceneJournalReviewSelected},
        {"diario_acoes", SceneJournalActions},
        {"diario_dia_hoje", SceneJournalTodayActive},
        {"diario_semana", [](uint8_t* fb) { SceneJournal(fb, journal_view::Level::kWeek); }},
        {"diario_mes", [](uint8_t* fb) { SceneJournal(fb, journal_view::Level::kMonth); }},
        {"diario_ano", [](uint8_t* fb) { SceneJournal(fb, journal_view::Level::kYear); }},
        {"detalhes", SceneDetails},
        {"livros", [](uint8_t* fb) { SceneBooks(fb, false); }},
        {"livros_vazio", [](uint8_t* fb) { SceneBooks(fb, true); }},
        {"leitor", SceneReader},
        {"toast", SceneToast},
        {"modal_desligar", SceneCardModal},
        {"sticky_note", SceneStickyNote},
    };
    for (int i = 0; i < kSlideCount; ++i) {
        scenes.push_back({"onboarding_" + std::to_string(i + 1),
                          [i](uint8_t* fb) { SceneOnboarding(fb, i); }});
    }
    return scenes;
}

// Raw buffer: 800x480, MSB-first, bit set = white. Portrait (x, y) lives at raw (y, 479 - x)
// (see DrawPortraitPixel in display_service.cpp). PBM uses bit set = black, so invert.
bool WritePortraitPbm(const Framebuffer& fb, const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    std::fprintf(file, "P4\n%d %d\n", kPortraitWidth, kPortraitHeight);
    std::vector<uint8_t> row(kPortraitWidth / 8);
    for (int y = 0; y < kPortraitHeight; ++y) {
        std::fill(row.begin(), row.end(), 0);
        for (int x = 0; x < kPortraitWidth; ++x) {
            const int raw_x = y;
            const int raw_y = kRawHeight - 1 - x;
            const size_t index = static_cast<size_t>(raw_y) * (kRawWidth / 8) + raw_x / 8;
            const bool white = (fb[index] & (0x80U >> (raw_x & 7))) != 0;
            if (!white) {
                row[x / 8] |= static_cast<uint8_t>(0x80U >> (x & 7));
            }
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}

}  // namespace

// Usage: screen_preview <output_dir> [scene-substring ...]
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output_dir> [filter ...]\n", argv[0]);
        return 2;
    }
    const std::string out_dir = argv[1];
    std::vector<std::string> filters(argv + 2, argv + argc);

    int written = 0;
    for (const Scene& scene : Scenes()) {
        bool wanted = filters.empty();
        for (const std::string& filter : filters) {
            wanted = wanted || scene.name.find(filter) != std::string::npos;
        }
        if (!wanted) {
            continue;
        }
        Framebuffer fb(kBufferLen, 0xFF);
        scene.draw(fb.data());
        const std::string path = out_dir + "/" + scene.name + ".pbm";
        if (!WritePortraitPbm(fb, path)) {
            std::fprintf(stderr, "failed to write %s\n", path.c_str());
            return 1;
        }
        std::printf("%s\n", scene.name.c_str());
        ++written;
    }
    return written > 0 ? 0 : 1;
}

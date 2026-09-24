import type {
  StatusType,
  TalkingClockModuleResponse,
  TalkingClockModuleSettings,
  TimeConfigFormValues,
  TimeRuntimeStatus,
  TimeSettingsResponse,
  TimezoneListResponse,
  ValidatableField,
} from './types';

interface TimeControllerDeps {
  applyTalkingClockModuleSettings: (
    settings?: TalkingClockModuleSettings
  ) => void;
  bedtimeTimeInput: ValidatableField;
  clearFieldError: (field: ValidatableField) => void;
  clockModeToggle: HTMLButtonElement;
  clockSyncPollAttempts: number;
  clockSyncPollIntervalMs: number;
  fetchTimeRuntimeJson: (
    path: string,
    init?: RequestInit
  ) => Promise<{ success: boolean; message?: string; runtime?: TimeRuntimeStatus }>;
  fetchTimeSettingsJson: (
    path: string,
    init?: RequestInit
  ) => Promise<TimeSettingsResponse>;
  fetchTimezoneListJson: (
    path: string,
    init?: RequestInit
  ) => Promise<TimezoneListResponse>;
  focusTalkingClockTimeInput: () => void;
  buildTimezoneLabelMap: (
    timezones: Array<{ name: string; description?: string }>
  ) => Map<string, string>;
  formatTimezoneLabel: (
    item: { name: string; description?: string },
    labelMap: Map<string, string>
  ) => string;
  isSwitchChecked: (toggle: HTMLButtonElement) => boolean;
  isTalkingClockModuleActive: () => boolean;
  isTalkingClockModuleBusy: () => boolean;
  manualDateInput: ValidatableField;
  manualTimeInput: ValidatableField;
  notifyClockMode: (message: string, type?: StatusType) => void;
  notify: (message: string, type?: StatusType) => void;
  onStateChange: () => void;
  onTalkingClockBusyChange: (busy: boolean) => void;
  parseClockTimeInputValue: (input: { value: string }) => number | null;
  patchTalkingClockSettings: (
    patch: Partial<TalkingClockModuleSettings>
  ) => Promise<TalkingClockModuleResponse>;
  setFieldError: (field: ValidatableField, message: string) => void;
  setSwitchChecked: (toggle: HTMLButtonElement, checked: boolean) => void;
  timeRuntimeApi: string;
  timeSettingsApi: string;
  timezoneSelect: ValidatableField;
  wakeupTimeInput: ValidatableField;
}

export function createTimeController(deps: TimeControllerDeps) {
  let isTimezoneBusy = false;
  let isLocationBusy = false;
  let isClockBusy = false;
  let isClockSyncRefreshPending = false;

  async function fetchTimezoneList(): Promise<TimezoneListResponse> {
    return deps.fetchTimezoneListJson('/api/timezone/list');
  }

  function applyTimeSettingsStatus(data: TimeSettingsResponse) {
    const settings = data.settings;
    const runtime = data.runtime;

    deps.setSwitchChecked(deps.clockModeToggle, Boolean(settings?.enabled));
    deps.timezoneSelect.value = settings?.timezone_name || '';
    deps.clearFieldError(deps.timezoneSelect);
    deps.manualDateInput.value =
      typeof runtime?.current_date === 'string' ? runtime.current_date : '';
    deps.manualTimeInput.value =
      typeof runtime?.current_time === 'string' ? runtime.current_time : '';
  }

  function applyTimeRuntimeStatus(runtime?: TimeRuntimeStatus) {
    if (!runtime) {
      return;
    }

    if (typeof runtime.clock_enabled === 'boolean') {
      deps.setSwitchChecked(deps.clockModeToggle, runtime.clock_enabled);
    }

    deps.manualDateInput.value =
      typeof runtime.current_date === 'string' ? runtime.current_date : '';
    deps.manualTimeInput.value =
      typeof runtime.current_time === 'string' ? runtime.current_time : '';
  }

  async function populateTimezoneOptions() {
    deps.timezoneSelect.innerHTML = '';
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = 'Escolha o fuso';
    deps.timezoneSelect.appendChild(placeholder);

    try {
      const data = await fetchTimezoneList();
      const timezones = Array.isArray(data.timezones) ? data.timezones : [];
      const labelMap = deps.buildTimezoneLabelMap(timezones);
      timezones
        .map(item => ({
          value: item.name,
          label: deps.formatTimezoneLabel(item, labelMap),
        }))
        .sort((a, b) => a.label.localeCompare(b.label))
        .forEach(option => {
          const element = document.createElement('option');
          element.value = option.value;
          element.textContent = option.label;
          deps.timezoneSelect.appendChild(element);
        });
    } catch (error) {
      console.error('Timezone list fetch failed:', error);
      deps.notify(
        error instanceof Error ? error.message : 'Falha ao carregar os fusos horários.',
        'error'
      );
    }
  }

  async function fetchTimeSettingsStatus() {
    if (isTimezoneBusy || isLocationBusy || isClockBusy) {
      return;
    }

    isTimezoneBusy = true;
    isLocationBusy = true;
    isClockBusy = true;
    deps.onStateChange();

    try {
      const data = await deps.fetchTimeSettingsJson(deps.timeSettingsApi);
      applyTimeSettingsStatus(data);
    } catch (error) {
      console.error('Time settings status failed:', error);
      deps.notify(
        error instanceof Error ? error.message : 'Falha ao carregar as configurações de data e hora.',
        'error'
      );
    } finally {
      isTimezoneBusy = false;
      isLocationBusy = false;
      isClockBusy = false;
      deps.onStateChange();
    }
  }

  async function refreshClockStatusAfterWifiConnect() {
    if (isClockBusy || isClockSyncRefreshPending) {
      return;
    }

    isClockSyncRefreshPending = true;

    try {
      for (let attempt = 0; attempt < deps.clockSyncPollAttempts; attempt++) {
        const data = await deps.fetchTimeRuntimeJson(deps.timeRuntimeApi);
        applyTimeRuntimeStatus(data.runtime);

        if (
          data.runtime?.time_valid === true &&
          typeof data.runtime.current_time === 'string' &&
          data.runtime.current_time.length > 0
        ) {
          return;
        }

        if (attempt < deps.clockSyncPollAttempts - 1) {
          await new Promise<void>(resolve => {
            setTimeout(resolve, deps.clockSyncPollIntervalMs);
          });
        }
      }
    } catch (error) {
      console.error('Clock refresh after WiFi connect failed:', error);
    } finally {
      isClockSyncRefreshPending = false;
    }
  }

  async function fetchTimeRuntimeStatus() {
    if (document.hidden || isTimezoneBusy || isLocationBusy || isClockBusy) {
      return;
    }

    if (!deps.isSwitchChecked(deps.clockModeToggle)) {
      return;
    }

    try {
      const data = await deps.fetchTimeRuntimeJson(deps.timeRuntimeApi);
      applyTimeRuntimeStatus(data.runtime);
    } catch (error) {
      console.error('Time runtime status failed:', error);
    }
  }

  function getTimeConfigFormValues(): TimeConfigFormValues | null {
    const timezoneName = deps.timezoneSelect.value.trim();
    const manualDate = deps.manualDateInput.value.trim();
    const manualTime = deps.manualTimeInput.value.trim();
    const wakeupMinutes = deps.parseClockTimeInputValue(deps.wakeupTimeInput);
    const bedtimeMinutes = deps.parseClockTimeInputValue(deps.bedtimeTimeInput);

    if (Boolean(manualDate) !== Boolean(manualTime)) {
      deps.notify(
        'Para ajustar manualmente, informe a data e a hora.',
        'error'
      );
      return null;
    }

    return {
      timezoneName,
      manualDate,
      manualTime,
      wakeupMinutes: wakeupMinutes ?? undefined,
      bedtimeMinutes: bedtimeMinutes ?? undefined,
    };
  }

  async function updateTimeSettings(
    formValues: TimeConfigFormValues,
    enabled: boolean
  ): Promise<TimeSettingsResponse> {
    return deps.fetchTimeSettingsJson(deps.timeSettingsApi, {
      method: 'PATCH',
      body: JSON.stringify({
        timezone_name: formValues.timezoneName,
        enabled,
        manual_date: formValues.manualDate,
        manual_time: formValues.manualTime,
      }),
    });
  }

  async function toggleClockModeSetting() {
    if (isClockBusy || isTimezoneBusy || isLocationBusy) {
      deps.notifyClockMode('Já existe uma atualização do relógio em andamento.', 'warning');
      return;
    }

    const previousEnabled = deps.isSwitchChecked(deps.clockModeToggle);
    const enabled = !previousEnabled;
    const timezoneName = deps.timezoneSelect.value.trim();
    deps.setSwitchChecked(deps.clockModeToggle, enabled);

    isClockBusy = true;
    deps.notifyClockMode(
      enabled ? 'Ativando o modo relógio...' : 'Desativando o modo relógio...',
      'info'
    );
    deps.onStateChange();

    try {
      const data = await deps.fetchTimeSettingsJson(deps.timeSettingsApi, {
        method: 'PATCH',
        body: JSON.stringify({
          enabled,
          timezone_name: enabled && timezoneName ? timezoneName : undefined,
        }),
      });
      applyTimeSettingsStatus(data);
      deps.notifyClockMode(
        data.message || (enabled ? 'Modo relógio ativado.' : 'Modo relógio desativado.'),
        'success'
      );
    } catch (error) {
      deps.setSwitchChecked(deps.clockModeToggle, previousEnabled);
      console.error('Clock mode toggle failed:', error);
      const errorMessage =
        error instanceof Error ? error.message : 'Falha ao atualizar o modo relógio.';
      const finalErrorMessage =
        errorMessage === 'timezone_name required to enable clock'
          ? 'Escolha o fuso horário primeiro.'
          : errorMessage;
      if (errorMessage === 'timezone_name required to enable clock') {
        deps.setFieldError(deps.timezoneSelect, 'Escolha o fuso horário.');
        deps.timezoneSelect.focus({ preventScroll: true });
      }
      deps.notifyClockMode(finalErrorMessage, 'error');
    } finally {
      isClockBusy = false;
      deps.onStateChange();
    }
  }

  async function saveTimezoneLocation() {
    if (isTimezoneBusy || isLocationBusy || isClockBusy || deps.isTalkingClockModuleBusy()) {
      deps.notify('Já existe uma atualização de data e hora em andamento.', 'warning');
      return;
    }

    const formValues = getTimeConfigFormValues();
    if (!formValues) {
      return;
    }
    if (!formValues.timezoneName) {
      deps.setFieldError(deps.timezoneSelect, 'Escolha o fuso horário.');
      deps.notify('Escolha o fuso horário.', 'error');
      deps.timezoneSelect.focus({ preventScroll: true });
      return;
    }
    deps.clearFieldError(deps.timezoneSelect);
    if (
      deps.isTalkingClockModuleActive() &&
      (formValues.wakeupMinutes === undefined || formValues.bedtimeMinutes === undefined)
    ) {
      deps.notify(
        'Os horários de acordar e de dormir são obrigatórios para o relógio falante.',
        'error'
      );
      deps.focusTalkingClockTimeInput();
      return;
    }

    isTimezoneBusy = true;
    isLocationBusy = true;
    isClockBusy = true;
    deps.onTalkingClockBusyChange(deps.isTalkingClockModuleActive());
    deps.notify('Salvando data e hora...', 'info');
    deps.onStateChange();

    try {
      const data = await updateTimeSettings(
        formValues,
        deps.isSwitchChecked(deps.clockModeToggle)
      );
      applyTimeSettingsStatus(data);

      if (deps.isTalkingClockModuleActive()) {
        const moduleData = await deps.patchTalkingClockSettings({
          wakeup_minutes: formValues.wakeupMinutes,
          bedtime_minutes: formValues.bedtimeMinutes,
        });
        deps.applyTalkingClockModuleSettings(moduleData.settings);
      }
      deps.notify('Data e hora salvas.', 'success');
    } catch (error) {
      console.error('Timezone/location save failed:', error);
      deps.notify(
        error instanceof Error ? error.message : 'Falha ao salvar data e hora.',
        'error'
      );
    } finally {
      isTimezoneBusy = false;
      isLocationBusy = false;
      isClockBusy = false;
      deps.onTalkingClockBusyChange(false);
      deps.onStateChange();
    }
  }

  async function clearTimezoneLocation() {
    if (isTimezoneBusy || isLocationBusy || isClockBusy) {
      deps.notify('Já existe uma atualização de data e hora em andamento.', 'warning');
      return;
    }

    isTimezoneBusy = true;
    isLocationBusy = true;
    isClockBusy = true;
    deps.notify('Limpando o fuso horário...', 'info');
    deps.onStateChange();

    try {
      const data = await deps.fetchTimeSettingsJson(deps.timeSettingsApi, {
        method: 'PATCH',
        body: JSON.stringify({
          enabled: false,
          timezone_name: '',
        }),
      });
      applyTimeSettingsStatus(data);
      deps.notify('Fuso horário removido.', 'success');
    } catch (error) {
      console.error('Timezone/location clear failed:', error);
      deps.notify(
        error instanceof Error ? error.message : 'Falha ao limpar o fuso horário.',
        'error'
      );
    } finally {
      isTimezoneBusy = false;
      isLocationBusy = false;
      isClockBusy = false;
      deps.onStateChange();
    }
  }

  return {
    applyTimeRuntimeStatus,
    applyTimeSettingsStatus,
    clearTimezoneLocation,
    fetchTimeRuntimeStatus,
    fetchTimeSettingsStatus,
    isClockBusy: () => isClockBusy,
    isLocationBusy: () => isLocationBusy,
    isTimezoneBusy: () => isTimezoneBusy,
    populateTimezoneOptions,
    refreshClockStatusAfterWifiConnect,
    saveTimezoneLocation,
    toggleClockModeSetting,
  };
}

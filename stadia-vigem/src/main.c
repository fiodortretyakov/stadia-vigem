/*
 * main.c -- Program entry point and Stadia device mapping.
 */

#include <stdio.h>
#include <string.h>
#include <tchar.h>
#include <windows.h>
#include <synchapi.h>

#include <ViGEm/Client.h>

#include "tray.h"
#include "hid.h"
#include "stadia.h"

#ifndef _DEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif

#define MAX_ACTIVE_DEVICE_COUNT 4
#define DEVICE_COUNT_TEMPLATE TEXT("%d/4 device(s) connected")

struct active_device
{
    struct hid_device *src_device;
    struct stadia_controller *controller;
    PVIGEM_TARGET tgt_device;
    XUSB_REPORT tgt_report;
    DWORD prev_buttons;
};

static int active_device_count = 0;
static struct active_device *active_devices[MAX_ACTIVE_DEVICE_COUNT];
static SRWLOCK active_devices_lock = SRWLOCK_INIT;
static PVIGEM_CLIENT vigem_client;
static BOOL vigem_connected = FALSE;

static struct tray_menu tray_menu_device_count;

// future declarations
static void stadia_controller_update_cb(struct stadia_controller *controller, struct stadia_state *state);
static void stadia_controller_stop_cb(struct stadia_controller *controller);
static void CALLBACK x360_notification_cb(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR large_motor,
                                          UCHAR small_motor, UCHAR led_number, LPVOID user_data);
static void refresh_cb(struct tray_menu *item);
static void quit_cb(struct tray_menu *item);

static const struct tray_menu tray_menu_refresh = {.text = TEXT("Refresh"), .cb = refresh_cb};
static const struct tray_menu tray_menu_quit = {.text = TEXT("Quit"), .cb = quit_cb};
static const struct tray_menu tray_menu_separator = {.text = TEXT("-")};
static const struct tray_menu tray_menu_terminator = {.text = NULL};
static struct tray tray =
    {
        .icon = TEXT("APP_ICON"),
        .tip = TEXT("Stadia Controller"),
        .menu = NULL};

SHORT FORCEINLINE _map_byte_to_short(BYTE value, BOOL inverted)
{
    CHAR centered = value - 128;
    if (centered < -127)
    {
        centered = -127;
    }
    if (inverted)
    {
        centered = -centered;
    }
    return (SHORT)(32767 * centered / 127);
}

/*
 * Keyboard shortcuts for the Capture and Assistant buttons, which have no Xbox 360 counterpart.
 *
 * They come from stadia-vigem.ini next to the program, read on every press, so edits apply at once:
 *
 *   [default]
 *   Capture=Win+PrintScreen
 *   CaptureHold=F9
 *
 *   [Prey.exe]
 *   Capture=F5
 *   Assistant=F9, hold 1s
 *
 * A section named after the foreground program's exe replaces [default] completely; without either, the button does
 * nothing. An empty value also means the button does nothing. ", hold 1s" (or ", hold 500ms") sends the keys only
 * after the button has been held that long.
 * <Button>Hold sets a second shortcut for holding the button (1 second unless it has its own ", hold"). With it, the
 * plain shortcut is sent on a short press, at release, and the hold shortcut when the button is still down after
 * the hold time; only one of the two is sent.
 */
#define SETTINGS_FILE_NAME TEXT("stadia-vigem.ini")
#define MAX_SHORTCUT_KEYS 4
#define SHORTCUT_KEY_DOWN_MS 50
#define DEFAULT_HOLD_MS 1000
#define HOLD_POLL_MS 10

enum extra_button
{
    EXTRA_BUTTON_CAPTURE,
    EXTRA_BUTTON_ASSISTANT,
    EXTRA_BUTTON_COUNT
};

static const DWORD extra_button_masks[EXTRA_BUTTON_COUNT] = {STADIA_BUTTON_CAPTURE, STADIA_BUTTON_ASSISTANT};
static const LPCTSTR extra_button_names[EXTRA_BUTTON_COUNT] = {TEXT("Capture"), TEXT("Assistant")};
static const LPCTSTR extra_button_hold_names[EXTRA_BUTTON_COUNT] = {TEXT("CaptureHold"), TEXT("AssistantHold")};

// Changes on every press and release, so a hold shortcut can tell whether the button is still down
static volatile LONG extra_button_press_ids[EXTRA_BUTTON_COUNT];

static TCHAR settings_path[MAX_PATH];

struct shortcut
{
    WORD keys[MAX_SHORTCUT_KEYS];
    int key_count;
    DWORD hold_ms;
};

struct pending_shortcut
{
    struct shortcut tap;
    struct shortcut hold;
    int button;
    LONG press_id;
};

static void trim(LPTSTR text)
{
    LPTSTR start = text;
    while (*start == TEXT(' ') || *start == TEXT('\t'))
    {
        start++;
    }
    size_t length = _tcslen(start);
    while (length > 0 && (start[length - 1] == TEXT(' ') || start[length - 1] == TEXT('\t')))
    {
        length--;
    }
    memmove(text, start, length * sizeof(TCHAR));
    text[length] = TEXT('\0');
}

static WORD key_from_name(LPCTSTR name)
{
    static const struct
    {
        LPCTSTR name;
        WORD vk;
    } named_keys[] = {
        {TEXT("Win"), VK_LWIN}, {TEXT("Ctrl"), VK_CONTROL}, {TEXT("Alt"), VK_MENU}, {TEXT("Shift"), VK_SHIFT},
        {TEXT("Esc"), VK_ESCAPE}, {TEXT("Enter"), VK_RETURN}, {TEXT("Space"), VK_SPACE}, {TEXT("Tab"), VK_TAB},
        {TEXT("Backspace"), VK_BACK}, {TEXT("Insert"), VK_INSERT}, {TEXT("Delete"), VK_DELETE},
        {TEXT("Home"), VK_HOME}, {TEXT("End"), VK_END}, {TEXT("PageUp"), VK_PRIOR}, {TEXT("PageDown"), VK_NEXT},
        {TEXT("Up"), VK_UP}, {TEXT("Down"), VK_DOWN}, {TEXT("Left"), VK_LEFT}, {TEXT("Right"), VK_RIGHT},
        {TEXT("PrintScreen"), VK_SNAPSHOT}};

    for (int i = 0; i < (int)ARRAYSIZE(named_keys); i++)
    {
        if (_tcsicmp(name, named_keys[i].name) == 0)
        {
            return named_keys[i].vk;
        }
    }
    if ((name[0] == TEXT('F') || name[0] == TEXT('f')) && _istdigit(name[1]))
    {
        int number = _ttoi(name + 1);
        return number >= 1 && number <= 24 ? (WORD)(VK_F1 + number - 1) : 0;
    }
    if (name[0] != TEXT('\0') && name[1] == TEXT('\0'))
    {
        TCHAR c = (TCHAR)_totupper(name[0]);
        if ((c >= TEXT('A') && c <= TEXT('Z')) || (c >= TEXT('0') && c <= TEXT('9')))
        {
            return (WORD)c;
        }
    }
    return 0;
}

// Parses "Ctrl+S" or "F9, hold 1s"; returns FALSE (and no keys) on anything it does not understand
static BOOL parse_shortcut(LPTSTR text, struct shortcut *shortcut)
{
    memset(shortcut, 0, sizeof(*shortcut));

    LPTSTR comment = _tcschr(text, TEXT(';'));
    if (comment != NULL)
    {
        *comment = TEXT('\0');
    }
    LPTSTR option = _tcschr(text, TEXT(','));
    if (option != NULL)
    {
        *option++ = TEXT('\0');
        trim(option);
        if (_tcsnicmp(option, TEXT("hold"), 4) != 0)
        {
            return FALSE;
        }
        LPTSTR unit = NULL;
        double amount = _tcstod(option + 4, &unit);
        trim(unit);
        if (amount <= 0)
        {
            return FALSE;
        }
        if (_tcsicmp(unit, TEXT("s")) == 0)
        {
            amount *= 1000;
        }
        else if (_tcsicmp(unit, TEXT("ms")) != 0)
        {
            return FALSE;
        }
        if (amount > 10000)
        {
            return FALSE;
        }
        shortcut->hold_ms = (DWORD)amount;
    }

    trim(text);
    LPTSTR context = NULL;
    for (LPTSTR part = _tcstok_s(text, TEXT("+"), &context); part != NULL; part = _tcstok_s(NULL, TEXT("+"), &context))
    {
        trim(part);
        WORD vk = key_from_name(part);
        if (vk == 0 || shortcut->key_count == MAX_SHORTCUT_KEYS)
        {
            shortcut->key_count = 0;
            return FALSE;
        }
        shortcut->keys[shortcut->key_count++] = vk;
    }
    return TRUE;
}

static void foreground_exe_name(LPTSTR name, DWORD size)
{
    name[0] = TEXT('\0');
    DWORD pid = 0;
    HWND window = GetForegroundWindow();
    if (window == NULL || GetWindowThreadProcessId(window, &pid) == 0)
    {
        return;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == NULL)
    {
        return;
    }
    TCHAR path[MAX_PATH];
    DWORD length = MAX_PATH;
    if (QueryFullProcessImageName(process, 0, path, &length))
    {
        LPTSTR base = _tcsrchr(path, TEXT('\\'));
        lstrcpyn(name, base != NULL ? base + 1 : path, size);
    }
    CloseHandle(process);
}

// The foreground program's own section if the file has one, otherwise [default]
static void settings_section(LPTSTR section, DWORD size)
{
    TCHAR exe[MAX_PATH];
    TCHAR keys[64];

    foreground_exe_name(exe, MAX_PATH);
    if (exe[0] != TEXT('\0') &&
        GetPrivateProfileString(exe, NULL, TEXT(""), keys, ARRAYSIZE(keys), settings_path) > 0)
    {
        lstrcpyn(section, exe, size);
        return;
    }
    lstrcpyn(section, TEXT("default"), size);
}

static void lookup_shortcuts(int button, struct shortcut *tap, struct shortcut *hold)
{
    TCHAR section[MAX_PATH];
    TCHAR value[128];

    settings_section(section, MAX_PATH);
    GetPrivateProfileString(section, extra_button_names[button], TEXT(""), value, ARRAYSIZE(value), settings_path);
    parse_shortcut(value, tap);
    GetPrivateProfileString(section, extra_button_hold_names[button], TEXT(""), value, ARRAYSIZE(value), settings_path);
    parse_shortcut(value, hold);
    if (hold->key_count > 0 && hold->hold_ms == 0)
    {
        hold->hold_ms = DEFAULT_HOLD_MS;
    }
}

static void fill_key_input(INPUT *input, WORD vk, BOOL up)
{
    // Scan codes too, because games that read raw input or DirectInput ignore virtual-key-only events
    UINT scan = MapVirtualKey(vk, MAPVK_VK_TO_VSC_EX);
    input->type = INPUT_KEYBOARD;
    input->ki.wVk = vk;
    input->ki.wScan = (WORD)(scan & 0xFF);
    input->ki.dwFlags = (scan != 0 ? KEYEVENTF_SCANCODE : 0) | ((scan & 0xFF00) == 0xE000 ? KEYEVENTF_EXTENDEDKEY : 0) |
                        (up ? KEYEVENTF_KEYUP : 0);
}

// Presses the keys in order, holds them briefly so games that poll once per frame see them, releases in reverse order
static void send_shortcut(const struct shortcut *shortcut)
{
    INPUT inputs[MAX_SHORTCUT_KEYS] = {0};

    for (int i = 0; i < shortcut->key_count; i++)
    {
        fill_key_input(&inputs[i], shortcut->keys[i], FALSE);
    }
    SendInput(shortcut->key_count, inputs, sizeof(INPUT));
    Sleep(SHORTCUT_KEY_DOWN_MS);
    for (int i = 0; i < shortcut->key_count; i++)
    {
        fill_key_input(&inputs[i], shortcut->keys[shortcut->key_count - 1 - i], TRUE);
    }
    SendInput(shortcut->key_count, inputs, sizeof(INPUT));
}

// TRUE while the button has not been released since this press
static BOOL still_pressed(const struct pending_shortcut *pending)
{
    return extra_button_press_ids[pending->button] == pending->press_id;
}

static DWORD WINAPI shortcut_thread(LPVOID param)
{
    struct pending_shortcut *pending = (struct pending_shortcut *)param;

    if (pending->hold.key_count > 0)
    {
        // Short press or hold: wait for the release or the hold time, whichever comes first, and send only one.
        ULONGLONG start = GetTickCount64();
        while (still_pressed(pending) && GetTickCount64() - start < pending->hold.hold_ms)
        {
            Sleep(HOLD_POLL_MS);
        }
        if (still_pressed(pending))
        {
            send_shortcut(&pending->hold);
        }
        else if (pending->tap.key_count > 0)
        {
            send_shortcut(&pending->tap);
        }
    }
    else
    {
        if (pending->tap.hold_ms > 0)
        {
            Sleep(pending->tap.hold_ms);
        }
        // A ", hold" shortcut fires only if the button has not been released since this press
        if (pending->tap.hold_ms == 0 || still_pressed(pending))
        {
            send_shortcut(&pending->tap);
        }
    }
    free(pending);
    return 0;
}

// Runs on the controller's input thread, so the key presses and holds happen on a thread of their own
static void start_shortcut(int button)
{
    LONG press_id = InterlockedIncrement(&extra_button_press_ids[button]);
    struct shortcut tap;
    struct shortcut hold;

    lookup_shortcuts(button, &tap, &hold);
    if (tap.key_count == 0 && hold.key_count == 0)
    {
        return;
    }
    struct pending_shortcut *pending = (struct pending_shortcut *)malloc(sizeof(struct pending_shortcut));
    pending->tap = tap;
    pending->hold = hold;
    pending->button = button;
    pending->press_id = press_id;
    HANDLE thread = CreateThread(NULL, 0, shortcut_thread, pending, 0, NULL);
    if (thread == NULL)
    {
        free(pending);
        return;
    }
    CloseHandle(thread);
}

static void init_settings_path()
{
    DWORD length = GetModuleFileName(NULL, settings_path, MAX_PATH);
    LPTSTR base = length > 0 && length < MAX_PATH ? _tcsrchr(settings_path, TEXT('\\')) : NULL;
    if (base == NULL || (size_t)(base + 1 - settings_path) + _tcslen(SETTINGS_FILE_NAME) >= MAX_PATH)
    {
        lstrcpyn(settings_path, SETTINGS_FILE_NAME, MAX_PATH);
        return;
    }
    lstrcpyn(base + 1, SETTINGS_FILE_NAME, (int)(MAX_PATH - (base + 1 - settings_path)));
}

static void rebuild_tray_menu()
{
    struct tray_menu *prev_menu = tray.menu;
    
    struct tray_menu *new_menu = (struct tray_menu *)malloc(5 * sizeof(struct tray_menu));
    int index = 0;

    AcquireSRWLockShared(&active_devices_lock);

    LPTSTR old_device_count_text = tray_menu_device_count.text;

    INT tray_text_length = _sctprintf(DEVICE_COUNT_TEMPLATE, active_device_count);
    tray_menu_device_count.text = (LPTSTR)malloc((tray_text_length + 1) * sizeof(TCHAR));
    _stprintf(tray_menu_device_count.text, DEVICE_COUNT_TEMPLATE, active_device_count);

    free(old_device_count_text);

    ReleaseSRWLockShared(&active_devices_lock);

    new_menu[index++] = tray_menu_device_count;
    new_menu[index++] = tray_menu_separator;
    new_menu[index++] = tray_menu_refresh;
    new_menu[index++] = tray_menu_quit;
    new_menu[index++] = tray_menu_terminator;

    tray.menu = new_menu;
    free(prev_menu);
}

static BOOL add_device(LPTSTR path)
{
    if (active_device_count == MAX_ACTIVE_DEVICE_COUNT)
    {
        tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia Controller error"),
                               TEXT("Device count limit reached"));
        return FALSE;
    }

    struct hid_device *device = hid_open_device(path, TRUE, FALSE);
    if (device == NULL)
    {
        if (hid_reenable_device(path))
        {
            device = hid_open_device(path, TRUE, FALSE);
            if (device == NULL)
            {
                device = hid_open_device(path, TRUE, TRUE);
            }
        }
        else
        {
            device = hid_open_device(path, TRUE, TRUE);
        }
    }

    if (device == NULL)
    {
        tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia Controller error"),
                               TEXT("Error opening new device"));
        return FALSE;
    }

    struct stadia_controller *controller = stadia_controller_create(device);
    if (controller == NULL)
    {
        tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia Controller error"),
                               TEXT("Error initializing new device"));
        hid_close_device(device);
        hid_free_device(device);
        return FALSE;
    }

    struct active_device *active_device = (struct active_device *)malloc(sizeof(struct active_device));
    active_device->src_device = device;
    active_device->controller = controller;
    active_device->prev_buttons = STADIA_BUTTON_NONE;

    if (vigem_connected)
    {
        active_device->tgt_device = vigem_target_x360_alloc();
        vigem_target_add(vigem_client, active_device->tgt_device);
        XUSB_REPORT_INIT(&active_device->tgt_report);
        vigem_target_x360_register_notification(vigem_client, active_device->tgt_device, x360_notification_cb,
                                                (LPVOID)active_device);
    }

    AcquireSRWLockExclusive(&active_devices_lock);
    active_devices[active_device_count++] = active_device;
    ReleaseSRWLockExclusive(&active_devices_lock);

    rebuild_tray_menu();
    tray_update(&tray);

    if (!vigem_connected)
    {
        tray_show_notification(NT_TRAY_WARNING, TEXT("Stadia Controller error"),
                               TEXT("Device added, but emulation doesn't work due to ViGEmBus problem"));
    }

    return TRUE;
}

// Frees a device that is no longer in active_devices. Runs on the main thread only.
static void release_device(struct active_device *device)
{
    if (vigem_connected)
    {
        vigem_target_x360_unregister_notification(device->tgt_device);
    }

    // Joins the controller's input and output threads, so no stadia callback uses the device after this.
    stadia_controller_destroy(device->controller);

    hid_close_device(device->src_device);
    hid_free_device(device->src_device);

    if (vigem_connected)
    {
        vigem_target_remove(vigem_client, device->tgt_device);
        vigem_target_free(device->tgt_device);
    }

    free(device);
}

static void refresh_devices()
{
    LPTSTR stadia_hw_path_filters[3] = {STADIA_USB_HW_FILTER, STADIA_BLT_HW_FILTER, NULL};
    struct hid_device_info *device_info = hid_enumerate(stadia_hw_path_filters);
    struct hid_device_info *cur;
    BOOL found = FALSE;

    // Take out devices that are gone or whose controller stopped (for example after a disconnect),
    // then release them outside the lock: releasing waits for the controller threads.
    struct active_device *stale_devices[MAX_ACTIVE_DEVICE_COUNT];
    int stale_count = 0;

    AcquireSRWLockExclusive(&active_devices_lock);

    for (int i = 0; i < active_device_count;)
    {
        found = FALSE;
        cur = device_info;

        while (cur != NULL)
        {
            if (_tcscmp(active_devices[i]->src_device->path, cur->path) == 0)
            {
                found = TRUE;
                break;
            }

            cur = cur->next;
        }

        if (found && active_devices[i]->controller->active)
        {
            i++;
            continue;
        }

        stale_devices[stale_count++] = active_devices[i];
        if (i < active_device_count - 1)
        {
            memmove(&active_devices[i], &active_devices[i + 1],
                    sizeof(struct active_device *) * (active_device_count - i - 1));
        }
        active_device_count--;
    }

    ReleaseSRWLockExclusive(&active_devices_lock);

    for (int i = 0; i < stale_count; i++)
    {
        release_device(stale_devices[i]);
    }

    if (stale_count > 0)
    {
        rebuild_tray_menu();
        tray_update(&tray);
    }

    // add new devices
    cur = device_info;
    while (cur != NULL)
    {
        found = FALSE;
        AcquireSRWLockShared(&active_devices_lock);
        for (int i = 0; i < active_device_count; i++)
        {
            if (_tcscmp(cur->path, active_devices[i]->src_device->path) == 0)
            {
                found = TRUE;
                break;
            }
        }
        ReleaseSRWLockShared(&active_devices_lock);
        if (!found)
        {
            add_device(cur->path);
        }
        cur = cur->next;
    }

    // free hid_device_info list
    while (device_info)
    {
        cur = device_info->next;
        hid_free_device_info(device_info);
        device_info = cur;
    }
}

static void device_change_cb(UINT op, LPTSTR path)
{
    refresh_devices();
}

static void stadia_controller_update_cb(struct stadia_controller *controller, struct stadia_state *state)
{
    struct active_device *active_device = NULL;
    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        if (active_devices[i]->controller == controller)
        {
            active_device = active_devices[i];
            break;
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);

    if (active_device == NULL)
    {
        return;
    }

    // Capture and Assistant have no Xbox 360 counterpart: they send keyboard shortcuts (see stadia-vigem.ini)
    DWORD pressed = state->buttons & ~active_device->prev_buttons;
    DWORD released = active_device->prev_buttons & ~state->buttons;
    active_device->prev_buttons = state->buttons;
    for (int button = 0; button < EXTRA_BUTTON_COUNT; button++)
    {
        if ((released & extra_button_masks[button]) != 0)
        {
            InterlockedIncrement(&extra_button_press_ids[button]);
        }
        if ((pressed & extra_button_masks[button]) != 0)
        {
            start_shortcut(button);
        }
    }

    if (vigem_connected)
    {
        active_device->tgt_report.wButtons = 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_UP) != 0 ? XUSB_GAMEPAD_DPAD_UP : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_DOWN) != 0 ? XUSB_GAMEPAD_DPAD_DOWN : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_LEFT) != 0 ? XUSB_GAMEPAD_DPAD_LEFT : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_RIGHT) != 0 ? XUSB_GAMEPAD_DPAD_RIGHT : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_MENU) != 0 ? XUSB_GAMEPAD_START : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_OPTIONS) != 0 ? XUSB_GAMEPAD_BACK : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_LS) != 0 ? XUSB_GAMEPAD_LEFT_THUMB : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_RS) != 0 ? XUSB_GAMEPAD_RIGHT_THUMB : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_LB) != 0 ? XUSB_GAMEPAD_LEFT_SHOULDER : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_RB) != 0 ? XUSB_GAMEPAD_RIGHT_SHOULDER : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_A) != 0 ? XUSB_GAMEPAD_A : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_B) != 0 ? XUSB_GAMEPAD_B : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_X) != 0 ? XUSB_GAMEPAD_X : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_Y) != 0 ? XUSB_GAMEPAD_Y : 0;
        active_device->tgt_report.wButtons |= (state->buttons & STADIA_BUTTON_STADIA_BTN) != 0 ? XUSB_GAMEPAD_GUIDE : 0;
        active_device->tgt_report.bLeftTrigger = state->left_trigger;
        active_device->tgt_report.bRightTrigger = state->right_trigger;
        active_device->tgt_report.sThumbLX = _map_byte_to_short(state->left_stick_x, FALSE);
        active_device->tgt_report.sThumbLY = _map_byte_to_short(state->left_stick_y, TRUE);
        active_device->tgt_report.sThumbRX = _map_byte_to_short(state->right_stick_x, FALSE);
        active_device->tgt_report.sThumbRY = _map_byte_to_short(state->right_stick_y, TRUE);
        vigem_target_x360_update(vigem_client, active_device->tgt_device, active_device->tgt_report);
    }
}

// Runs on the controller's input thread: only ask the main thread to refresh, which releases the stopped device.
static void stadia_controller_stop_cb(struct stadia_controller *controller)
{
    (void)controller;
    tray_request_device_refresh();
}

// Runs on a ViGEm thread that can still fire after its device was released, so look the device up by target
// instead of trusting user_data, and hold the list lock while using it.
static void CALLBACK x360_notification_cb(PVIGEM_CLIENT client, PVIGEM_TARGET target, UCHAR large_motor,
                                          UCHAR small_motor, UCHAR led_number, LPVOID user_data)
{
    (void)client;
    (void)led_number;
    (void)user_data;

    AcquireSRWLockShared(&active_devices_lock);
    for (int i = 0; i < active_device_count; i++)
    {
        if (active_devices[i]->tgt_device == target)
        {
            stadia_controller_set_vibration(active_devices[i]->controller, small_motor, large_motor);
            break;
        }
    }
    ReleaseSRWLockShared(&active_devices_lock);
}

static void refresh_cb(struct tray_menu *item)
{
    (void)item;
    fflush(stdout);
    refresh_devices();
}

static void quit_cb(struct tray_menu *item)
{
    (void)item;
    fflush(stdout);
    tray_exit();
}

INT main()
{
    init_settings_path();
    rebuild_tray_menu();
    if (tray_init(&tray) < 0)
    {
        printf("Failed to create tray\n");
        return 1;
    }
    vigem_client = vigem_alloc();
    VIGEM_ERROR vigem_res = vigem_connect(vigem_client);
    if (vigem_res == VIGEM_ERROR_BUS_NOT_FOUND)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("Stadia Controller error"),
                               TEXT("ViGEmBus not installed"));
    }
    else if (vigem_res == VIGEM_ERROR_BUS_VERSION_MISMATCH)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("Stadia Controller error"),
                               TEXT("ViGEmBus incompatible version"));
    }
    else if (vigem_res != VIGEM_ERROR_NONE)
    {
        tray_show_notification(NT_TRAY_ERROR, TEXT("Stadia Controller error"),
                               TEXT("Error connecting to ViGEmBus"));
    }
    else
    {
        vigem_connected = TRUE;
    }

    stadia_update_callback = stadia_controller_update_cb;
    stadia_stop_callback = stadia_controller_stop_cb;

    // Register first, so a controller that connects during the first scan is not missed.
    tray_register_device_notification(hid_get_class(), device_change_cb);
    refresh_devices();

    while (tray_loop(TRUE) == 0)
    {
        ;
    }

    AcquireSRWLockExclusive(&active_devices_lock);
    INT count = active_device_count;
    struct active_device *remaining[MAX_ACTIVE_DEVICE_COUNT];
    memcpy(remaining, active_devices, sizeof(struct active_device *) * count);
    active_device_count = 0;
    ReleaseSRWLockExclusive(&active_devices_lock);
    for (INT i = 0; i < count; i++)
    {
        release_device(remaining[i]);
    }
    if (vigem_connected)
    {
        vigem_disconnect(vigem_client);
    }
    vigem_free(vigem_client);
    free(tray.menu);
    return 0;
}

//! The crate against a fake host table: what reaches Anvil, what a panic in
//! an adapter does, and what `Drop` removes.
#![allow(clippy::undocumented_unsafe_blocks)]

use std::ffi::{c_char, c_void, CStr};
use std::ptr;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};

use crate::sys::*;
use crate::{
    commands, game, input, log_throttle, map, panel, script, world, Change, ErrorCode,
    FunctionHook, Host, Payload, Plugin, Tag,
};

/// One test at a time: the host is global to the crate, as in a plugin DLL.
static SERIAL: Mutex<()> = Mutex::new(());

#[derive(Default)]
struct Recorded {
    log: Vec<String>,
    removed: Vec<usize>,
    mid_hooks: Vec<(usize, usize)>, // (callback, user)
    subscriptions: Vec<(String, usize, usize)>,
    rows: Vec<String>,
    clicks: Vec<(usize, usize)>,
    map_infos: Vec<String>,
    describes: Vec<(usize, usize)>,
    colors: Vec<u32>,
    bindings: Vec<(i64, u32, usize, usize)>, // code, flags, handler, context
    day_ends: Vec<(usize, usize, usize)>,    // (self, callback, context)
    effects: Vec<(usize, String, usize, usize, usize)>, // (self, key, execute, describe, context)
    commands: Vec<String>,
}

static RECORDED: Mutex<Option<Recorded>> = Mutex::new(None);
static NEXT_CHANGE: AtomicUsize = AtomicUsize::new(0x1000);

fn with<R>(f: impl FnOnce(&mut Recorded) -> R) -> R {
    f(RECORDED
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get_or_insert_with(Recorded::default))
}

fn text(p: *const c_char) -> String {
    unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned()
}

unsafe extern "C" fn fake_log(_: *mut AnvilPlugin, _: AnvilLogLevel, message: *const c_char) {
    let line = text(message);
    with(|r| r.log.push(line));
}

unsafe extern "C" fn fake_original(x: i32) -> i32 {
    x + 1
}

unsafe extern "C" fn fake_hook_function(
    _: *mut AnvilPlugin,
    symbol: AnvilSymbol,
    _detour: *mut c_void,
    next: *mut *mut c_void,
    out: *mut *mut AnvilChange,
) -> AnvilResult {
    if symbol == 99 {
        return ANVIL_ERROR_NOT_DECLARED;
    }
    let original: unsafe extern "C" fn(i32) -> i32 = fake_original;
    unsafe {
        *next = original as *mut c_void;
        *out = NEXT_CHANGE.fetch_add(1, Ordering::Relaxed) as *mut AnvilChange;
    }
    ANVIL_OK
}

unsafe extern "C" fn fake_mid_hook(
    _: *mut AnvilPlugin,
    _site: AnvilSymbol,
    callback: AnvilCpuCallback,
    user: *mut c_void,
    out: *mut *mut AnvilChange,
) -> AnvilResult {
    let change = NEXT_CHANGE.fetch_add(1, Ordering::Relaxed);
    with(|r| {
        r.mid_hooks
            .push((callback.map_or(0, |f| f as usize), user as usize))
    });
    unsafe { *out = change as *mut AnvilChange };
    ANVIL_OK
}

unsafe extern "C" fn fake_remove(_: *mut AnvilPlugin, change: *mut AnvilChange) -> AnvilResult {
    with(|r| r.removed.push(change as usize));
    ANVIL_OK
}

unsafe extern "C" fn fake_subscribe(
    _: *mut AnvilPlugin,
    event: *const c_char,
    callback: AnvilEventCallback,
    context: *mut c_void,
) -> AnvilResult {
    let name = text(event);
    with(|r| {
        r.subscriptions
            .push((name, callback.map_or(0, |f| f as usize), context as usize))
    });
    ANVIL_OK
}

unsafe extern "C" fn fake_publish(
    _: *mut AnvilPlugin,
    _: *const c_char,
    _: *const c_void,
) -> AnvilResult {
    ANVIL_OK
}

unsafe extern "C" fn fake_get_service(
    _: *mut AnvilPlugin,
    name: *const c_char,
    version: u32,
) -> *const c_void {
    match (text(name).as_str(), version) {
        ("anvil-overlay.panel", 1) => ptr::from_ref(&*FAKE_PANEL).cast(),
        ("anvil-game.world", 1) if FULL_WORLD.load(Ordering::Relaxed) => {
            ptr::from_ref(&*FAKE_WORLD_FULL).cast()
        }
        ("anvil-game.world", 1) => ptr::from_ref(&*FAKE_WORLD).cast(),
        ("anvil-map.modes", 1) => ptr::from_ref(&*FAKE_MAP).cast(),
        ("anvil-overlay.input", 1) => ptr::from_ref(&*FAKE_INPUT).cast(),
        ("anvil-script.day_end", 1) => ptr::from_ref(&*FAKE_DAY_END).cast(),
        ("anvil-script.effects", 1) => ptr::from_ref(&*FAKE_EFFECTS).cast(),
        ("anvil-game.commands", 1 | 2) => ptr::from_ref(&*FAKE_COMMANDS).cast(),
        ("anvil-log-throttle.messages", 1) => ptr::from_ref(&*FAKE_LOG_MESSAGES).cast(),
        _ => ptr::null(),
    }
}

// --- a fake input service ------------------------------------------------------

unsafe extern "C" fn input_bind(
    code: i64,
    flags: u32,
    handler: AnvilInputHandler,
    context: *mut c_void,
    out: *mut *mut AnvilInputBinding,
) -> AnvilResult {
    if code == 0x77 {
        // As the overlay does: the inert binding, with the conflict.
        unsafe { *out = 0x98 as *mut AnvilInputBinding };
        return ANVIL_ERROR_CONFLICT;
    }
    let handler = handler.map_or(0, |f| f as usize);
    with(|r| r.bindings.push((code, flags, handler, context as usize)));
    unsafe { *out = 0x99 as *mut AnvilInputBinding };
    ANVIL_OK
}

unsafe extern "C" fn input_held(code: i64) -> core::ffi::c_int {
    core::ffi::c_int::from(code == 0x241)
}

static FAKE_INPUT: Shared<AnvilInputService> = Shared({
    // SAFETY: a table of Option<fn>: all zero is all None.
    let mut table: AnvilInputService = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilInputService>() as u32;
    table.bind = Some(input_bind);
    table.held = Some(input_held);
    table
});

// --- a fake commands service ----------------------------------------------------

// "<self> <type> <flags> <id>/<tag>/<kind> ..."; a count of 0 is refused, and a
// drill start skips one.
unsafe extern "C" fn commands_post(
    plugin: *mut AnvilPlugin,
    command: *const AnvilGameCommand,
    out: *mut AnvilGameCommandResult,
) -> AnvilResult {
    let c = unsafe { &*command };
    if c.type_ == ANVIL_GAME_COMMAND_EVENT_OPTION {
        let line = format!(
            "{:x} event {} {} size {}",
            plugin as usize, c.pending_event_id, c.option_index, c.size
        );
        with(|r| r.commands.push(line));
        let out = unsafe { &mut *out };
        out.posted = 1;
        return ANVIL_OK;
    }
    if (c.size as usize) < std::mem::offset_of!(AnvilGameCommand, pending_event_id) || c.count == 0
    {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    let units = unsafe { std::slice::from_raw_parts(c.units, c.count as usize) };
    let mut line = format!("{:x} {} {}", plugin as usize, c.type_, c.flags);
    for u in units {
        let tag: String = u.country.text[..3]
            .iter()
            .map(|&b| b as u8 as char)
            .collect();
        line += &format!(" {:x}/{tag}/{}", u.id, u.kind);
    }
    with(|r| r.commands.push(line));
    let out = unsafe { &mut *out };
    let skipped =
        u32::from(c.type_ == ANVIL_GAME_COMMAND_DRILL && c.flags == ANVIL_GAME_COMMAND_START);
    out.posted = c.count - skipped;
    out.skipped = skipped;
    out.refused = 0;
    ANVIL_OK
}

static FAKE_COMMANDS: Shared<AnvilGameCommandsService> = Shared(AnvilGameCommandsService {
    size: std::mem::size_of::<AnvilGameCommandsService>() as u32,
    post: Some(commands_post),
});

// --- a fake anvil-log-throttle.messages ------------------------------------------

static LOG_KEPT: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn log_keep() -> AnvilResult {
    LOG_KEPT.store(1, Ordering::SeqCst);
    ANVIL_OK
}

unsafe extern "C" fn log_totals(out: *mut AnvilLogTotals) -> AnvilResult {
    let out = unsafe { &mut *out };
    if out.size < std::mem::size_of::<AnvilLogTotals>() as u32 {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    out.keeping = LOG_KEPT.load(Ordering::SeqCst);
    out.version = 2500;
    out.messages = 2500;
    out.written = 2600;
    out.counted = 7;
    ANVIL_OK
}

// 2500 messages: message i has version i + 1, i written lines and, for the
// first, 7 counted ones.
unsafe extern "C" fn log_read(
    first: u64,
    changed_after: u64,
    out: *mut AnvilLogMessage,
    capacity: u32,
    message_size: u32,
    count: *mut u32,
    next: *mut u64,
) -> AnvilResult {
    if message_size < std::mem::size_of::<AnvilLogMessage>() as u32 {
        return ANVIL_ERROR_INVALID_ARGUMENT;
    }
    let mut n = 0u32;
    let mut i = first;
    while i < 2500 && n < capacity {
        if i + 1 > changed_after {
            let m = unsafe { &mut *out.add(n as usize) };
            m.index = i;
            m.version = i + 1;
            m.written = i;
            m.counted = if i == 0 { 7 } else { 0 };
            m.log = c"error.log".as_ptr();
            m.source = c"gamestate.cpp".as_ptr();
            m.source_line = 7653;
            m.text = c"Undefined event target: destination_province".as_ptr();
            n += 1;
        }
        i += 1;
    }
    unsafe {
        *count = n;
        *next = i;
    }
    ANVIL_OK
}

static FAKE_LOG_MESSAGES: Shared<AnvilLogMessagesService> = Shared(AnvilLogMessagesService {
    size: std::mem::size_of::<AnvilLogMessagesService>() as u32,
    keep: Some(log_keep),
    totals: Some(log_totals),
    read: Some(log_read),
});

// --- a fake panel service -------------------------------------------------------

unsafe extern "C" fn panel_create(_: *const c_char, out: *mut *mut AnvilPanel) -> AnvilResult {
    unsafe { *out = 0x77 as *mut AnvilPanel };
    ANVIL_OK
}

unsafe extern "C" fn panel_set_rows(
    _: *mut AnvilPanel,
    rows: *const AnvilPanelRow,
    count: u32,
    size: u32,
) -> AnvilResult {
    assert_eq!(size as usize, std::mem::size_of::<AnvilPanelRow>());
    let rows = unsafe { std::slice::from_raw_parts(rows, count as usize) };
    let lines: Vec<String> = rows
        .iter()
        .map(|row| {
            let label = if row.label.is_null() {
                String::new()
            } else {
                text(row.label)
            };
            let value = if row.value.is_null() {
                String::new()
            } else {
                format!("={}", text(row.value))
            };
            format!("{}:{label}{value}:{}", row.kind, row.id)
        })
        .collect();
    with(|r| r.rows = lines);
    ANVIL_OK
}

unsafe extern "C" fn panel_on_click(
    _: *mut AnvilPanel,
    click: AnvilPanelClick,
    context: *mut c_void,
) -> AnvilResult {
    with(|r| {
        r.clicks
            .push((click.map_or(0, |f| f as usize), context as usize))
    });
    ANVIL_OK
}

unsafe extern "C" fn panel_show(_: *mut AnvilPanel, _: core::ffi::c_int) -> AnvilResult {
    ANVIL_OK
}

struct Shared<T>(T);
// SAFETY: test tables of function pointers, never written.
unsafe impl<T> Sync for Shared<T> {}
impl<T> std::ops::Deref for Shared<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.0
    }
}

static FAKE_PANEL: Shared<AnvilPanelService> = Shared(AnvilPanelService {
    size: std::mem::size_of::<AnvilPanelService>() as u32,
    create: Some(panel_create),
    set_rows: Some(panel_set_rows),
    on_click: Some(panel_on_click),
    show: Some(panel_show),
    shown: None,
    language: None,
});

// --- a fake map modes service: one mode, 0x88 --------------------------------------

const MAP_MODE: *const AnvilMapMode = 0x88 as *const AnvilMapMode;

unsafe extern "C" fn map_register(
    info: *const AnvilMapModeInfo,
    out: *mut *const AnvilMapMode,
) -> AnvilResult {
    let info = unsafe { &*info };
    assert_eq!(info.size as usize, std::mem::size_of::<AnvilMapModeInfo>());
    let optional = |p: *const c_char| if p.is_null() { String::new() } else { text(p) };
    let id = text(info.id);
    if id == "p.taken" {
        return ANVIL_ERROR_ALREADY_EXISTS;
    }
    with(|r| {
        r.map_infos.push(format!(
            "{}:{id}:{}:{}:{}",
            info.tab,
            text(info.label),
            optional(info.description),
            optional(info.sprite)
        ));
        if let Some(describe) = info.describe {
            r.describes.push((describe as usize, info.context as usize));
        }
    });
    unsafe { *out = MAP_MODE };
    ANVIL_OK
}

unsafe extern "C" fn map_set_colors(
    mode: *const AnvilMapMode,
    colors: *const u32,
    count: u32,
) -> AnvilResult {
    assert_eq!(mode, MAP_MODE);
    let colors = unsafe { std::slice::from_raw_parts(colors, count as usize) };
    with(|r| r.colors = colors.to_vec());
    ANVIL_OK
}

unsafe extern "C" fn map_mode_id(number: i32) -> *const c_char {
    if number == 80 {
        c"p.cores".as_ptr()
    } else {
        ptr::null()
    }
}

unsafe extern "C" fn map_shown() -> *const AnvilMapMode {
    MAP_MODE
}

unsafe extern "C" fn map_id(mode: *const AnvilMapMode) -> *const c_char {
    if mode == MAP_MODE {
        c"p.cores".as_ptr()
    } else {
        ptr::null()
    }
}

static FAKE_MAP: Shared<AnvilMapModesService> = Shared(AnvilMapModesService {
    size: std::mem::size_of::<AnvilMapModesService>() as u32,
    register_mode: Some(map_register),
    set_colors: Some(map_set_colors),
    mode_id: Some(map_mode_id),
    shown: Some(map_shown),
    id: Some(map_id),
});

// --- a fake day end service: refuses after two subscriptions ----------------------

unsafe extern "C" fn day_end_subscribe(
    plugin: *mut AnvilPlugin,
    callback: AnvilScriptDayEnd,
    context: *mut c_void,
) -> AnvilResult {
    with(|r| {
        if r.day_ends.len() == 2 {
            return ANVIL_ERROR_WRONG_PHASE;
        }
        r.day_ends.push((
            plugin as usize,
            callback.map_or(0, |f| f as usize),
            context as usize,
        ));
        ANVIL_OK
    })
}

static FAKE_DAY_END: Shared<AnvilScriptDayEndService> = Shared(AnvilScriptDayEndService {
    size: std::mem::size_of::<AnvilScriptDayEndService>() as u32,
    subscribe: Some(day_end_subscribe),
});

// --- a fake effects service: copies the key, refuses one that is taken -----------------

unsafe extern "C" fn effects_register(
    plugin: *mut AnvilPlugin,
    effect: *const AnvilScriptEffect,
) -> AnvilResult {
    let effect = unsafe { &*effect };
    let key = text(effect.key);
    with(|r| {
        if r.effects.iter().any(|e| e.1 == key) {
            return ANVIL_ERROR_ALREADY_EXISTS;
        }
        r.effects.push((
            plugin as usize,
            key,
            effect.execute.map_or(0, |f| f as usize),
            effect.describe.map_or(0, |f| f as usize),
            effect.context as usize,
        ));
        ANVIL_OK
    })
}

static FAKE_EFFECTS: Shared<AnvilScriptEffectsService> = Shared(AnvilScriptEffectsService {
    size: std::mem::size_of::<AnvilScriptEffectsService>() as u32,
    register_effect: Some(effects_register),
});

// --- a fake world: a province name that grows between the calls, a list ------------

static NAME_CALLS: AtomicU32 = AtomicU32::new(0);

unsafe extern "C" fn world_province_name(
    id: i32,
    utf8: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    if id != 295 {
        return ANVIL_ERROR_NOT_FOUND;
    }
    // The first answer says 3 bytes, then the name is longer: the crate asks again.
    let name: &[u8] = if NAME_CALLS.fetch_add(1, Ordering::Relaxed) == 0 {
        b"Mos"
    } else {
        b"Moskva"
    };
    unsafe {
        *length = name.len() as u32;
        let fits = name.len().min(capacity.saturating_sub(1) as usize);
        if capacity > 0 {
            ptr::copy_nonoverlapping(name.as_ptr(), utf8.cast::<u8>(), fits);
            *utf8.add(fits) = 0;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_provinces(
    tag: *const c_char,
    ids: *mut i32,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    if text(tag) != "RUS" {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let all = [295, 296, 310];
    unsafe {
        *count = all.len() as u32;
        for (i, id) in all.iter().take(capacity as usize).enumerate() {
            *ids.add(i) = *id;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country(tag: *const c_char, out: *mut AnvilGameCountry) -> AnvilResult {
    let size = unsafe { (*out).size };
    assert_eq!(size as usize, std::mem::size_of::<AnvilGameCountry>());
    let mut c: AnvilGameCountry = unsafe { std::mem::zeroed() };
    c.size = size;
    c.tag = Tag::new(&text(tag)).unwrap().to_raw();
    c.capital = 295;
    c.province_count = 3;
    c.values = ANVIL_GAME_VALUE_TREASURY;
    c.treasury = 123_450;
    c.stability = 1000; // without its bit: not known on this build
    unsafe { *out = c };
    ANVIL_OK
}

static FAKE_WORLD: Shared<AnvilGameWorldService> = Shared({
    // SAFETY: a table of Option<fn>: all zero is all None.
    let mut table: AnvilGameWorldService = unsafe { std::mem::zeroed() };
    // Only up to war_side: the 1.15 calls are not there.
    table.size = (std::mem::offset_of!(AnvilGameWorldService, war_side) + 8) as u32;
    table.province_name = Some(world_province_name);
    table.country_provinces = Some(world_country_provinces);
    table.country = Some(world_country);
    table
});

// --- a fake world of anvil-game 1.20: a ruler, flags, variable names -----------

static FULL_WORLD: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

unsafe extern "C" fn world_monarch(
    tag: *const c_char,
    kind: u32,
    out: *mut AnvilGameMonarch,
) -> AnvilResult {
    if text(tag) != "RUS" || kind == ANVIL_GAME_MONARCH_HEIR {
        return ANVIL_ERROR_NOT_FOUND;
    }
    let size = unsafe { (*out).size };
    assert_eq!(size as usize, std::mem::size_of::<AnvilGameMonarch>());
    let mut m: AnvilGameMonarch = unsafe { std::mem::zeroed() };
    m.size = size;
    m.kind = kind;
    m.id = 0x1234_0000_0030;
    m.country = Tag::new("RUS").unwrap().to_raw();
    m.flags = ANVIL_GAME_MONARCH_FEMALE;
    m.adm = 6;
    m.dip = 4;
    m.mil = 5;
    m.birth.year = 1672;
    m.birth.month = 6;
    m.birth.day = 9;
    m.birth.session = 1;
    unsafe { *out = m };
    ANVIL_OK
}

unsafe extern "C" fn world_global_flags(
    out: *mut AnvilGameFlag,
    capacity: u32,
    count: *mut u32,
) -> AnvilResult {
    unsafe {
        *count = 1;
        if capacity > 0 {
            let mut f: AnvilGameFlag = std::mem::zeroed();
            f.id = 2;
            f.date.year = 1700;
            f.date.month = 8;
            f.date.day = 19;
            *out = f;
        }
    }
    ANVIL_OK
}

unsafe extern "C" fn world_country_variable_names(
    _tag: *const c_char,
    names: *mut c_char,
    capacity: u32,
    length: *mut u32,
) -> AnvilResult {
    let all = b"treasury_plan\0war_goal\0";
    unsafe {
        *length = all.len() as u32;
        if capacity as usize >= all.len() {
            ptr::copy_nonoverlapping(all.as_ptr(), names.cast::<u8>(), all.len());
        }
    }
    ANVIL_OK
}

static FAKE_WORLD_FULL: Shared<AnvilGameWorldService> = Shared({
    let mut table: AnvilGameWorldService = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilGameWorldService>() as u32;
    table.monarch = Some(world_monarch);
    table.global_flags = Some(world_global_flags);
    table.country_variable_names = Some(world_country_variable_names);
    table
});

/// A fresh host table and record; the crate's host is this one afterwards.
fn fake_host() -> (MutexGuard<'static, ()>, &'static AnvilHost) {
    let guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
    FULL_WORLD.store(false, Ordering::Relaxed);
    *RECORDED.lock().unwrap_or_else(|e| e.into_inner()) = Some(Recorded::default());
    let mut table: AnvilHost = unsafe { std::mem::zeroed() };
    table.size = std::mem::size_of::<AnvilHost>() as u32;
    table.api_version = ANVIL_API_VERSION;
    table.anvil_version = c"9.9.9".as_ptr();
    table.log = Some(fake_log);
    table.hook_function = Some(fake_hook_function);
    table.mid_hook = Some(fake_mid_hook);
    table.remove = Some(fake_remove);
    table.subscribe = Some(fake_subscribe);
    table.publish = Some(fake_publish);
    table.get_service = Some(fake_get_service);
    (guard, Box::leak(Box::new(table)))
}

const SELF: *mut AnvilPlugin = 0x5E1F as *mut AnvilPlugin;

fn load<P: Plugin>(table: &'static AnvilHost) -> AnvilResult {
    unsafe { crate::__private::load::<P>(table, SELF) }
}

fn logged() -> Vec<String> {
    with(|r| r.log.clone())
}

struct Quiet;
impl Plugin for Quiet {
    fn load(_: &mut Host) -> crate::Result<Self> {
        Ok(Quiet)
    }
}

#[test]
fn load_refuses_a_missing_or_foreign_host() {
    let (_guard, table) = fake_host();
    assert_eq!(
        unsafe { crate::__private::load::<Quiet>(ptr::null(), SELF) },
        ANVIL_ERROR_INVALID_ARGUMENT
    );
    let mut old = *table;
    old.api_version = 1;
    assert_eq!(
        load::<Quiet>(Box::leak(Box::new(old))),
        ANVIL_ERROR_UNSUPPORTED
    );
    let mut short = *table;
    short.size = std::mem::offset_of!(AnvilHost, publish) as u32;
    assert_eq!(
        load::<Quiet>(Box::leak(Box::new(short))),
        ANVIL_ERROR_UNSUPPORTED
    );
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
}

#[test]
fn an_error_from_load_is_its_result_and_a_log_line() {
    struct Refuses;
    impl Plugin for Refuses {
        fn load(host: &mut Host) -> crate::Result<Self> {
            host.hook(&HOOK, 99, add_two)?;
            Ok(Refuses)
        }
    }
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    unsafe extern "C" fn add_two(x: i32) -> i32 {
        x + 2
    }
    let (_guard, table) = fake_host();
    assert_eq!(load::<Refuses>(table), ANVIL_ERROR_NOT_DECLARED);
    assert_eq!(
        logged(),
        ["load() failed: hook_function symbol 99: not declared in symbols.toml or settings.toml"]
    );
    assert!(!HOOK.is_active());
}

#[test]
fn a_panic_in_load_is_logged_with_its_place_and_does_not_unwind() {
    struct Panics;
    impl Plugin for Panics {
        fn load(_: &mut Host) -> crate::Result<Self> {
            panic!("on purpose")
        }
    }
    let (_guard, table) = fake_host();
    assert_eq!(load::<Panics>(table), ANVIL_ERROR_INTERNAL);
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("load() panicked: on purpose at "),
        "{log:?}"
    );
    assert!(log[0].contains("tests.rs"), "{log:?}");
}

#[test]
fn a_function_hook_gets_next_and_holds_one_change() {
    static HOOK: FunctionHook<unsafe extern "C" fn(i32) -> i32> = FunctionHook::new();
    unsafe extern "C" fn detour(x: i32) -> i32 {
        unsafe { HOOK.next().unwrap()(x) * 10 }
    }
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    assert!(HOOK.next().is_none());
    host.hook(&HOOK, 1, detour).unwrap();
    assert!(HOOK.is_active());
    assert_eq!(unsafe { detour(4) }, 50);
    let again = host.hook(&HOOK, 1, detour).unwrap_err();
    assert_eq!(again.code(), ErrorCode::InvalidArgument);
    HOOK.remove().unwrap();
    assert!(!HOOK.is_active());
    assert_eq!(with(|r| r.removed.len()), 1);
}

#[test]
fn dropping_a_change_removes_it_and_keep_keeps_it() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let dropped = host.mid_hook(1, |_| {}).unwrap();
    let id = dropped.as_raw() as usize;
    drop(dropped);
    assert_eq!(with(|r| r.removed.clone()), [id]);
    host.mid_hook(1, |_| {}).unwrap().keep();
    let removed: Change = host.mid_hook(1, |_| {}).unwrap();
    let removed_id = removed.as_raw() as usize;
    removed.remove().unwrap();
    assert_eq!(with(|r| r.removed.clone()), [id, removed_id]);
}

#[test]
fn a_panic_in_a_mid_hook_is_logged_once_and_the_game_carries_on() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    host.mid_hook(5, |cpu| {
        assert!(cpu.rcx != 13, "unlucky {}", cpu.rcx);
        // SAFETY: a test context.
        unsafe { cpu.regs_mut().rax = cpu.rcx + 1 };
    })
    .unwrap()
    .keep();
    let (callback, user) = with(|r| r.mid_hooks[0]);
    let callback: unsafe extern "C" fn(*mut AnvilCpuContext, *mut c_void) =
        unsafe { std::mem::transmute(callback) };
    let mut cpu: AnvilCpuContext = unsafe { std::mem::zeroed() };
    for rcx in [1, 13, 13, 2] {
        cpu.rcx = rcx;
        cpu.rax = 0;
        unsafe { callback(&mut cpu, user as *mut c_void) };
        assert_eq!(cpu.rax, if rcx == 13 { 0 } else { rcx + 1 });
    }
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("mid_hook symbol 5: the callback panicked (unlucky 13 at "),
        "{log:?}"
    );
}

#[test]
fn an_event_reads_only_the_size_its_publisher_knows() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let seen = std::sync::Arc::new(Mutex::new(Vec::new()));
    let sink = seen.clone();
    host.subscribe(game::SESSION, move |session: &game::Session| {
        sink.lock().unwrap().push(*session)
    })
    .unwrap();
    let (name, callback, context) = with(|r| r.subscriptions[0].clone());
    assert_eq!(name, "anvil-game.session");
    let callback: unsafe extern "C" fn(*const c_char, *const c_void, *mut c_void) =
        unsafe { std::mem::transmute(callback) };
    // An older publisher that knows only size and change; ironman is set behind it.
    let raw = AnvilGameSession {
        size: 8,
        change: ANVIL_GAME_SESSION_STARTED as u32,
        ironman: 1,
        multiplayer: 1,
        number: 7,
    };
    unsafe {
        callback(
            c"anvil-game.session".as_ptr(),
            ptr::from_ref(&raw).cast(),
            context as *mut c_void,
        )
    };
    let raw = AnvilGameSession {
        size: 24,
        change: ANVIL_GAME_SESSION_ENDED as u32,
        ironman: 1,
        multiplayer: 0,
        number: 2,
    };
    unsafe {
        callback(
            c"anvil-game.session".as_ptr(),
            ptr::from_ref(&raw).cast(),
            context as *mut c_void,
        )
    };
    assert_eq!(
        *seen.lock().unwrap(),
        [
            game::Session {
                change: game::SessionChange::Started,
                ironman: false,
                multiplayer: false,
                number: 0
            },
            game::Session {
                change: game::SessionChange::Ended,
                ironman: true,
                multiplayer: false,
                number: 2
            },
        ]
    );
}

#[test]
fn panel_rows_reach_the_overlay_and_a_panicking_click_is_logged_once() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let panels = panel::Panels::get(&host).unwrap();
    assert_eq!(panels.language(), "en"); // no language() in this table
    let p = panels.create("Test").unwrap();
    p.set_rows(&[
        panel::Row::value("Treasury", "12.5"),
        panel::Row::Separator,
        panel::Row::button("Ref\0resh", 3),
        panel::Row::text("plain"),
    ])
    .unwrap();
    assert_eq!(
        with(|r| r.rows.clone()),
        ["1:Treasury=12.5:0", "3::0", "2:Refresh:3", "1:plain:0"]
    );
    p.on_click(|id| assert!(id != 3, "button {id}")).unwrap();
    let (click, context) = with(|r| r.clicks[0]);
    let click: unsafe extern "C" fn(*mut c_void, u64) = unsafe { std::mem::transmute(click) };
    for id in [1, 3, 3] {
        unsafe { click(context as *mut c_void, id) };
    }
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("panel click handler: the callback panicked (button 3 at "),
        "{log:?}"
    );
}

#[test]
fn world_reads_grow_their_buffers_and_old_services_answer_unsupported() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let w = world::World::get(&host).unwrap();
    NAME_CALLS.store(0, Ordering::Relaxed);
    assert_eq!(w.province_name(295).unwrap(), "Moskva");
    assert_eq!(w.province_name(1).unwrap_err().code(), ErrorCode::NotFound);
    let rus = Tag::new("RUS").unwrap();
    assert_eq!(w.country_provinces(rus).unwrap(), [295, 296, 310]);
    let country = w.country(rus).unwrap();
    assert_eq!(country.capital, Some(295));
    assert_eq!(country.treasury, Some(world::Milli(123_450)));
    assert_eq!(country.stability, None);
    // war_ids is in the table's size but not set; the 1.15 calls are past it.
    assert_eq!(w.war_ids().unwrap_err().code(), ErrorCode::Unsupported);
    assert_eq!(
        w.selected_units().unwrap_err().code(),
        ErrorCode::Unsupported
    );
    assert!(game::Game::get(&host).is_none());
}

#[test]
fn world_reads_rulers_flags_and_variable_names() {
    let (_guard, table) = fake_host();
    FULL_WORLD.store(true, Ordering::Relaxed);
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let w = world::World::get(&host).unwrap();
    let rus = Tag::new("RUS").unwrap();
    let ruler = w.monarch(rus, world::MonarchKind::Ruler).unwrap();
    assert_eq!(ruler.kind, world::MonarchKind::Ruler);
    assert_eq!((ruler.adm, ruler.dip, ruler.mil), (6, 4, 5));
    assert!(ruler.female && !ruler.regency);
    assert_eq!(ruler.birth.to_string(), "1672.6.9");
    assert_eq!(ruler.leader, None);
    assert_eq!(
        w.monarch(rus, world::MonarchKind::Heir).unwrap_err().code(),
        ErrorCode::NotFound
    );
    let flags = w.global_flags().unwrap();
    assert_eq!(flags.len(), 1);
    assert_eq!(flags[0].id, world::FlagId(2));
    assert_eq!(flags[0].date.to_string(), "1700.8.19");
    assert_eq!(
        w.country_variable_names(rus).unwrap(),
        ["treasury_plan", "war_goal"]
    );
    // Set in the table's size but not filled in: Unsupported, like an older anvil-game.
    assert_eq!(w.flag_id("x").unwrap_err().code(), ErrorCode::Unsupported);
    FULL_WORLD.store(false, Ordering::Relaxed);
}

#[test]
fn input_binds_and_delivers_presses_through_its_adapter() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let i = input::Input::get(&host).unwrap();
    static PRESSES: Mutex<Vec<input::Press>> = Mutex::new(Vec::new());
    let flags = input::BindFlags {
        consume: true,
        repeat: false,
    };
    let binding = i
        .bind(crate::Key(0x24B), flags, |press| {
            PRESSES.lock().unwrap().push(press)
        })
        .unwrap();
    assert_eq!(binding.as_raw() as usize, 0x99);
    assert_eq!(
        i.bind(crate::Key(0x77), flags, |_| {}).unwrap_err().code(),
        ErrorCode::Conflict
    );
    let (inert, conflict) = i.bind_or_inert(crate::Key(0x77), flags, |_| {}).unwrap();
    assert_eq!(inert.as_raw() as usize, 0x98);
    assert_eq!(conflict.unwrap().code(), ErrorCode::Conflict);
    let (held, conflict) = i.bind_or_inert(crate::Key(0x24C), flags, |_| {}).unwrap();
    assert_eq!(held.as_raw() as usize, 0x99);
    assert!(conflict.is_none());
    let (code, raw_flags, handler, context) = with(|r| r.bindings[0]);
    assert_eq!((code, raw_flags), (0x24B, ANVIL_INPUT_CONSUME));
    let handler: unsafe extern "C" fn(*mut c_void, *const AnvilInputEvent) =
        unsafe { std::mem::transmute(handler) };
    let event = AnvilInputEvent {
        size: std::mem::size_of::<AnvilInputEvent>() as u32,
        flags: ANVIL_INPUT_EVENT_REPEAT,
        code: 0x24B,
        x: 10,
        y: 20,
        frame: 7,
    };
    unsafe { handler(context as *mut c_void, &event) };
    let presses = PRESSES.lock().unwrap().clone();
    assert_eq!(
        presses,
        [input::Press {
            code: crate::Key(0x24B),
            repeat: true,
            x: 10,
            y: 20,
            frame: 7
        }]
    );
    assert!(i.held(crate::Key(0x241)) && !i.held(crate::Key(0x242)));
    // state, set_code and unbind are past nothing but unset: Unsupported.
    assert!(i.state().is_none());
    assert_eq!(binding.unbind().unwrap_err().code(), ErrorCode::Unsupported);
}

#[test]
fn commands_post_for_this_plugin_with_the_units_as_given() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let c = commands::Commands::get(&host).unwrap();
    let rus = Tag::new("RUS");
    let army = world::UnitRef {
        id: 0x36,
        country: rus,
        kind: world::UnitKind::Army,
    };
    let navy = world::UnitRef {
        id: 0x37,
        country: rus,
        kind: world::UnitKind::Navy,
    };
    assert_eq!(
        c.drill(&[army, army], true).unwrap(),
        commands::Posted {
            posted: 1,
            skipped: 1,
            refused: 0
        }
    );
    assert_eq!(c.split(&navy).unwrap().posted, 1);
    assert_eq!(c.version(), 2);
    assert_eq!(c.choose_event_option(41, 2).unwrap().posted, 1);
    assert_eq!(c.merge(&[]).unwrap_err().code(), ErrorCode::InvalidArgument);
    assert_eq!(
        with(|r| r.commands.clone()),
        [
            "5e1f 1 1 36/RUS/0 36/RUS/0",
            "5e1f 3 0 37/RUS/1",
            "5e1f event 41 2 size 32"
        ]
    );
    assert_eq!(commands::MAX_UNITS, 64);
    assert_eq!(
        ErrorCode::from_result(ANVIL_ERROR_LIMIT),
        Some(ErrorCode::Limit)
    );
    assert_eq!(ErrorCode::Suspended.result(), ANVIL_ERROR_SUSPENDED);
}

#[test]
fn map_modes_register_paint_and_cut_tooltips_at_a_whole_character() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let modes = map::MapModes::get(&host).unwrap();
    let info = map::ModeInfo {
        tab: map::Tab::Trade,
        description: Some("Yours"),
        ..map::ModeInfo::new("p.cores", "Cores")
    };
    let mode = modes
        .register_with_tooltip(&info, |_, province| match province {
            1 => Some("Krak\u{f3}w \u{17c}\nline".to_string()),
            2 => panic!("province 2"),
            _ => None,
        })
        .unwrap();
    assert_eq!(with(|r| r.map_infos.clone()), ["3:p.cores:Cores:Yours:"]);
    modes
        .register(&map::ModeInfo::new("p.plain", "Plain"))
        .unwrap();
    assert_eq!(
        with(|r| (r.map_infos[1].clone(), r.describes.len())),
        ("0:p.plain:Plain::".to_string(), 1)
    );
    assert_eq!(
        modes
            .register(&map::ModeInfo::new("p.taken", "X"))
            .unwrap_err()
            .code(),
        ErrorCode::AlreadyExists
    );

    modes
        .set_colors(mode, &[map::NO_COLOR, map::rgb(1, 2, 3)])
        .unwrap();
    assert_eq!(with(|r| r.colors.clone()), [0xFF00_0000, 0x0001_0203]);
    assert_eq!(
        (modes.mode_id(80), modes.mode_id(1)),
        (Some("p.cores"), None)
    );
    assert_eq!(
        (modes.shown(), modes.id(mode)),
        (Some(mode), Some("p.cores"))
    );

    let (describe, context) = with(|r| r.describes[0]);
    let describe: unsafe extern "C" fn(
        *mut c_void,
        *const AnvilMapMode,
        i32,
        *mut c_char,
        usize,
    ) -> usize = unsafe { std::mem::transmute(describe) };
    let context = context as *mut c_void;
    // "Kraków " is 8 bytes and "ż" 2 more: 9 fit, so the cut is before "ż".
    let mut buffer = [0x55 as c_char; 10];
    assert_eq!(
        unsafe { describe(context, mode.as_raw(), 1, buffer.as_mut_ptr(), 10) },
        8
    );
    assert_eq!(text(buffer.as_ptr()), "Krak\u{f3}w ");
    let mut wide = [0 as c_char; 64];
    assert_eq!(
        unsafe { describe(context, mode.as_raw(), 1, wide.as_mut_ptr(), 64) },
        15
    );
    assert_eq!(
        unsafe { describe(context, mode.as_raw(), 3, wide.as_mut_ptr(), 64) },
        0
    );
    for _ in 0..2 {
        assert_eq!(
            unsafe { describe(context, mode.as_raw(), 2, wide.as_mut_ptr(), 64) },
            0
        );
    }
    let log = logged();
    assert_eq!(log.len(), 1, "{log:?}");
    assert!(
        log[0].starts_with("map mode tooltip: the callback panicked (province 2 at "),
        "{log:?}"
    );

    let change = AnvilMapModeChange {
        size: std::mem::size_of::<AnvilMapModeChange>() as u32,
        reserved: 0,
        mode: MAP_MODE,
        id: c"p.cores".as_ptr(),
    };
    let read = unsafe { map::ModeChange::read(ptr::from_ref(&change).cast()) }.unwrap();
    assert_eq!((read.mode, read.id), (Some(mode), Some("p.cores")));
    let old = AnvilMapModeChange { size: 8, ..change };
    let read = unsafe { map::ModeChange::read(ptr::from_ref(&old).cast()) }.unwrap();
    assert_eq!((read.mode, read.id), (None, None));
}

#[test]
fn milli_tags_and_errors_read_as_people_write_them() {
    assert_eq!(world::Milli(4500).to_string(), "4.5");
    assert_eq!(world::Milli(-12000).to_string(), "-12");
    assert_eq!(world::Milli(125).to_string(), "0.125");
    assert_eq!(world::Milli(-1).to_string(), "-0.001");
    assert_eq!(
        Tag::new("RUS").map(|t| t.to_string()).as_deref(),
        Some("RUS")
    );
    assert!(Tag::new("RU").is_none() && Tag::new("RUSS").is_none() && Tag::new("R-S").is_none());
    assert_eq!(Tag::from_raw(&AnvilGameTag { text: [0; 4] }), None);
    let error = crate::Error::new(ErrorCode::Conflict, "hook_function symbol 0");
    assert_eq!(
        error.to_string(),
        "hook_function symbol 0: conflict with another change"
    );
    assert_eq!(ErrorCode::from_result(ANVIL_OK), None);
    assert_eq!(ErrorCode::from_result(42), Some(ErrorCode::Other(42)));
    assert_eq!(ErrorCode::Other(0).result(), ANVIL_ERROR_INTERNAL);
}

#[test]
fn event_and_service_names_are_the_headers() {
    let pairs: [(&CStr, &[u8]); 12] = [
        (crate::PLUGINS_LOADED.name(), ANVIL_EVENT_PLUGINS_LOADED),
        (crate::SUSPENDED.name(), ANVIL_EVENT_SUSPENDED),
        (crate::RESUMED.name(), ANVIL_EVENT_RESUMED),
        (game::SESSION.name(), ANVIL_GAME_EVENT_SESSION),
        (game::WAR.name(), ANVIL_GAME_EVENT_WAR),
        (game::SERVICE, ANVIL_GAME_SERVICE),
        (world::SERVICE, ANVIL_GAME_WORLD_SERVICE),
        (commands::SERVICE, ANVIL_GAME_COMMANDS_SERVICE),
        (panel::SERVICE, ANVIL_PANEL_SERVICE),
        (input::SERVICE, ANVIL_INPUT_SERVICE),
        (crate::benchmark::SERVICE, ANVIL_BENCHMARK_SERVICE),
        (map::SERVICE, ANVIL_MAP_MODES_SERVICE),
    ];
    for (ours, header) in pairs {
        assert_eq!(ours.to_bytes_with_nul(), header);
    }
    for (ours, header) in [
        (game::READY.name(), ANVIL_GAME_EVENT_READY.as_slice()),
        (game::FRAME.name(), ANVIL_GAME_EVENT_FRAME),
        (game::MAIN_MENU.name(), ANVIL_GAME_EVENT_MAIN_MENU),
        (game::OOS.name(), ANVIL_GAME_EVENT_OOS),
        (game::DAY.name(), ANVIL_GAME_EVENT_DAY),
        (game::MONTH.name(), ANVIL_GAME_EVENT_MONTH),
        (game::YEAR.name(), ANVIL_GAME_EVENT_YEAR),
        (game::PLAYER.name(), ANVIL_GAME_EVENT_PLAYER),
        (crate::benchmark::QUIT.name(), ANVIL_BENCHMARK_EVENT_QUIT),
        (map::SHOWN.name(), ANVIL_MAP_EVENT_SHOWN),
        (map::HIDDEN.name(), ANVIL_MAP_EVENT_HIDDEN),
    ] {
        assert_eq!(ours.to_bytes_with_nul(), header);
    }
}

#[test]
fn day_end_subscribes_with_the_plugin_and_passes_the_day() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let days = script::DayEnd::get(&host).unwrap();
    static SEEN: Mutex<Vec<script::Day>> = Mutex::new(Vec::new());
    SEEN.lock().unwrap().clear();
    days.subscribe(&host, |day| SEEN.lock().unwrap().push(*day))
        .unwrap();
    days.subscribe(&host, |day| {
        if day.new_year {
            panic!("a new year");
        }
    })
    .unwrap();
    assert_eq!(
        days.subscribe(&host, |_| {}).unwrap_err().code(),
        ErrorCode::WrongPhase
    );
    let subscribed = with(|r| r.day_ends.clone());
    assert_eq!(subscribed.len(), 2);
    assert_eq!(subscribed[0].0, SELF as usize);

    let call = |index: usize, raw: &AnvilScriptDay| {
        let (_, callback, context) = subscribed[index];
        let callback: unsafe extern "C" fn(*mut c_void, *const AnvilScriptDay) =
            unsafe { std::mem::transmute(callback) };
        unsafe { callback(context as *mut c_void, raw) };
    };
    let new_year = AnvilScriptDay {
        size: std::mem::size_of::<AnvilScriptDay>() as u32,
        flags: ANVIL_SCRIPT_DAY_NEW_MONTH | ANVIL_SCRIPT_DAY_NEW_YEAR,
        year: 1445,
        month: 1,
        day: 1,
    };
    call(0, &new_year);
    assert_eq!(
        *SEEN.lock().unwrap(),
        [script::Day {
            year: 1445,
            month: 1,
            day: 1,
            new_month: true,
            new_year: true
        }]
    );
    // A panic stays in the adapter and is logged once.
    call(1, &new_year);
    call(1, &new_year);
    let panics = logged()
        .iter()
        .filter(|l| l.starts_with("day end: the callback panicked"))
        .count();
    assert_eq!(panics, 1);
    let ordinary = AnvilScriptDay {
        flags: 0,
        day: 2,
        ..new_year
    };
    call(1, &ordinary);
    call(0, &ordinary);
    assert_eq!(
        SEEN.lock().unwrap()[1],
        script::Day {
            year: 1445,
            month: 1,
            day: 2,
            new_month: false,
            new_year: false
        }
    );
}

fn raw_call(key: &CStr, value: &CStr, province: i32, country: &[u8; 4]) -> AnvilScriptCall {
    AnvilScriptCall {
        size: std::mem::size_of::<AnvilScriptCall>() as u32,
        reserved: 0,
        key: key.as_ptr(),
        value: value.as_ptr(),
        scope: AnvilScriptScope {
            size: std::mem::size_of::<AnvilScriptScope>() as u32,
            province,
            country: country.map(|b| b as c_char),
        },
    }
}

#[test]
fn effects_register_with_the_plugin_and_pass_the_call() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let effects = script::Effects::get(&host).unwrap();
    type Seen = (String, String, Option<i32>, Option<Tag>);
    static SEEN: Mutex<Vec<Seen>> = Mutex::new(Vec::new());
    SEEN.lock().unwrap().clear();
    effects
        .register_with_tooltip(
            &host,
            "chisel_set_terrain",
            |call| {
                SEEN.lock().unwrap().push((
                    call.key.to_owned(),
                    call.value.clone().into_owned(),
                    call.province,
                    call.country,
                ));
                if call.value == "boom" {
                    panic!("a bad value");
                }
            },
            |call| match call.value.as_ref() {
                "none" => None,
                "boom" => panic!("a bad tooltip"),
                value => Some(format!("Terrain becomes {value}\nignored")),
            },
        )
        .unwrap();
    effects
        .register(&host, "chisel_reset_terrain", |_| {})
        .unwrap();
    // A taken key, and a key the C API cannot carry (no call made).
    assert_eq!(
        effects
            .register(&host, "chisel_set_terrain", |_| {})
            .unwrap_err()
            .code(),
        ErrorCode::AlreadyExists
    );
    assert_eq!(
        effects
            .register(&host, "chisel\0x", |_| {})
            .unwrap_err()
            .code(),
        ErrorCode::InvalidArgument
    );

    let registered = with(|r| r.effects.clone());
    assert_eq!(registered.len(), 2);
    assert_eq!(registered[0].0, SELF as usize);
    assert_eq!(registered[0].1, "chisel_set_terrain");
    assert_ne!(registered[0].3, 0); // a describe callback
    assert_eq!(registered[1].1, "chisel_reset_terrain");
    assert_eq!(registered[1].3, 0); // none: no tooltip line

    let execute = |index: usize, call: &AnvilScriptCall| {
        let (_, _, f, _, context) = registered[index].clone();
        let f: unsafe extern "C" fn(*mut c_void, *const AnvilScriptCall) =
            unsafe { std::mem::transmute(f) };
        unsafe { f(context as *mut c_void, call) };
    };
    let describe = |call: &AnvilScriptCall, capacity: usize| -> (u32, String) {
        let (_, _, _, f, context) = registered[0].clone();
        let f: unsafe extern "C" fn(*mut c_void, *const AnvilScriptCall, *mut c_char, u32) -> u32 =
            unsafe { std::mem::transmute(f) };
        let mut buffer = vec![0x55u8; capacity.max(1)];
        let whole = unsafe {
            f(
                context as *mut c_void,
                call,
                if capacity == 0 {
                    ptr::null_mut()
                } else {
                    buffer.as_mut_ptr().cast()
                },
                capacity as u32,
            )
        };
        let end = buffer.iter().position(|&b| b == 0).unwrap_or(0);
        (whole, String::from_utf8(buffer[..end].to_vec()).unwrap())
    };

    // A province scope and a country scope; a NULL call is ignored.
    execute(
        0,
        &raw_call(c"chisel_set_terrain", c"mountain", 295, b"\0\0\0\0"),
    );
    execute(0, &raw_call(c"chisel_set_terrain", c"hills", 0, b"RUS\0"));
    {
        let (_, _, f, _, context) = registered[0].clone();
        let f: unsafe extern "C" fn(*mut c_void, *const AnvilScriptCall) =
            unsafe { std::mem::transmute(f) };
        unsafe { f(context as *mut c_void, ptr::null()) };
    }
    assert_eq!(
        *SEEN.lock().unwrap(),
        [
            (
                "chisel_set_terrain".to_owned(),
                "mountain".to_owned(),
                Some(295),
                None
            ),
            (
                "chisel_set_terrain".to_owned(),
                "hills".to_owned(),
                None,
                Tag::new("RUS")
            ),
        ]
    );

    // The first line only, its whole length, cut at a whole character.
    let call = raw_call(c"chisel_set_terrain", c"desert", 183, b"\0\0\0\0");
    assert_eq!(
        describe(&call, 64),
        (22, "Terrain becomes desert".to_owned())
    );
    assert_eq!(describe(&call, 8), (22, "Terrain".to_owned()));
    assert_eq!(describe(&call, 0).0, 22);
    let wide = raw_call(
        c"chisel_set_terrain",
        c"\xc3\xa9t\xc3\xa9",
        183,
        b"\0\0\0\0",
    );
    // "Terrain becomes été": 16 + 5 bytes; 18 bytes of room cut before the second byte of é.
    assert_eq!(describe(&wide, 18), (21, "Terrain becomes ".to_owned()));
    assert_eq!(
        describe(
            &raw_call(c"chisel_set_terrain", c"none", 1, b"\0\0\0\0"),
            64
        )
        .0,
        0
    );

    // Panics stay in the adapters and are logged once per callback.
    let boom = raw_call(c"chisel_set_terrain", c"boom", 1, b"\0\0\0\0");
    execute(0, &boom);
    execute(0, &boom);
    assert_eq!(describe(&boom, 64).0, 0);
    assert_eq!(describe(&boom, 64).0, 0);
    let log = logged();
    assert_eq!(
        log.iter()
            .filter(|l| l.starts_with("script effect: the callback panicked"))
            .count(),
        1
    );
    assert_eq!(
        log.iter()
            .filter(|l| l.starts_with("script effect tooltip: the callback panicked"))
            .count(),
        1
    );
}

#[test]
fn log_messages_keep_totals_and_what_changed() {
    let (_guard, table) = fake_host();
    assert_eq!(load::<Quiet>(table), ANVIL_OK);
    let host = Host::current().unwrap();
    let m = log_throttle::Messages::get(&host).unwrap();
    m.keep().unwrap();
    let totals = m.totals().unwrap();
    assert!(totals.keeping);
    assert_eq!(
        (
            totals.version,
            totals.messages,
            totals.written,
            totals.counted
        ),
        (2500, 2500, 2600, 7)
    );
    let (part, next) = m.read(0, 0, 3).unwrap();
    assert_eq!(next, 3);
    assert_eq!(part.len(), 3);
    assert_eq!(part[0].count(), 7);
    assert_eq!(part[0].text, "Undefined event target: destination_province");
    assert_eq!(
        (
            part[0].log.as_str(),
            part[0].source.as_str(),
            part[0].source_line
        ),
        ("error.log", "gamestate.cpp", 7653)
    );
    // The whole table in pieces, and only what changed after a version.
    assert_eq!(m.changed_since(0).unwrap().len(), 2500);
    let changed = m.changed_since(2400).unwrap();
    assert_eq!(changed.len(), 100);
    assert_eq!(changed[0].index, 2400);
    assert!(m.read(0, 0, 0).unwrap().0.is_empty());
}

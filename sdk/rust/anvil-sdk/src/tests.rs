//! The crate against a fake host table: what reaches Anvil, what a panic in
//! an adapter does, and what `Drop` removes.
#![allow(clippy::undocumented_unsafe_blocks)]

use std::ffi::{c_char, c_void, CStr};
use std::ptr;
use std::sync::atomic::{AtomicU32, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};

use crate::sys::*;
use crate::{
    commands, game, map, panel, world, Change, ErrorCode, FunctionHook, Host, Payload, Plugin, Tag,
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
        ("anvil-game.world", 1) => ptr::from_ref(&*FAKE_WORLD).cast(),
        ("anvil-map.modes", 1) => ptr::from_ref(&*FAKE_MAP).cast(),
        ("anvil-game.commands", 1 | 2) => ptr::from_ref(&*FAKE_COMMANDS).cast(),
        _ => ptr::null(),
    }
}

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

/// A fresh host table and record; the crate's host is this one afterwards.
fn fake_host() -> (MutexGuard<'static, ()>, &'static AnvilHost) {
    let guard = SERIAL.lock().unwrap_or_else(|e| e.into_inner());
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
    let pairs: [(&CStr, &[u8]); 11] = [
        (crate::PLUGINS_LOADED.name(), ANVIL_EVENT_PLUGINS_LOADED),
        (crate::SUSPENDED.name(), ANVIL_EVENT_SUSPENDED),
        (crate::RESUMED.name(), ANVIL_EVENT_RESUMED),
        (game::SESSION.name(), ANVIL_GAME_EVENT_SESSION),
        (game::WAR.name(), ANVIL_GAME_EVENT_WAR),
        (game::SERVICE, ANVIL_GAME_SERVICE),
        (world::SERVICE, ANVIL_GAME_WORLD_SERVICE),
        (commands::SERVICE, ANVIL_GAME_COMMANDS_SERVICE),
        (panel::SERVICE, ANVIL_PANEL_SERVICE),
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

#!/usr/bin/env python3
"""
Creates the first version of the EEZ Studio project
components/hmi/Esp32_monitor.eez-project (LVGL 8.3, 800x480, EEZ Flow).

This is a one-shot scaffold: after it ran, the .eez-project is the source of
truth and is edited in EEZ Studio. Running it again refuses to overwrite the
project unless --force is given (that throws away every change made in EEZ
Studio).

    python3 tools/eez_scaffold.py [--force] && tools/eez-build.sh

Widget JSON comes from tools/eez_templates.json (taken from projects saved by
EEZ Studio 0.29), with fresh objIDs. The firmware only depends on the names of
the global variables, native actions and screens defined below
(components/hmi/bridge/*), not on the layout.
"""
import argparse
import copy
import json
import os
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATES = os.path.join(HERE, 'eez_templates.json')
PROJECT = os.path.join(HERE, '..', 'components', 'hmi', 'Esp32_monitor.eez-project')

# ------------------------------------------------------------------ look
BG = '#0b0f14'
SIDEBAR = '#111821'
PANEL = '#161e28'
BORDER = '#253041'
ACCENT = '#2d7ff9'
ACCENT_DARK = '#1b3a66'
BTN = '#22303f'
DANGER = '#8a2a2a'
WHITE = '#ffffff'
GREY = '#8a96a3'
GREEN = '#3ecf6e'

SIDEBAR_W = 120
NAV_STEP = 47                # sidebar button pitch (10 pages in 480 px)
NAV_H = 42
CX = SIDEBAR_W + 10          # content x
CW = 800 - CX - 10           # content width
TOP = 48                     # content y

# LVGL symbols (Montserrat built-in fonts include them)
SYM_HOME = ''
SYM_WIFI = ''
SYM_BT = ''
SYM_CAN = ''           # LV_SYMBOL_SHUFFLE
SYM_IO = ''            # LV_SYMBOL_CHARGE
SYM_DISP = ''          # LV_SYMBOL_SETTINGS
SYM_GPS = '\uf124'      # LV_SYMBOL_GPS
SYM_ROUTE = '\uf03e'    # LV_SYMBOL_IMAGE
SYM_CLIMB = '\uf077'    # LV_SYMBOL_UP
SYM_RIDE = '\uf04b'     # LV_SYMBOL_PLAY

PAGES = [  # screen name, sidebar text, title
    ('home', SYM_HOME + '\nHome', 'System'),
    ('wifi', SYM_WIFI + '\nWi-Fi', 'Wi-Fi'),
    ('ble', SYM_BT + '\nBLE', 'Bluetooth LE'),
    ('can', SYM_CAN + '\nCAN', 'CAN bus'),
    ('io', SYM_IO + '\nIO', 'IO / ADC'),
    ('gps', SYM_GPS + '\nGPS', 'GPS'),
    ('route', SYM_ROUTE + '\nRoute', 'Route'),
    ('profile', SYM_CLIMB + '\nClimbs', 'Elevation'),
    ('ride', SYM_RIDE + '\nRide', 'Ride'),
    ('display', SYM_DISP + '\nDisplay', 'Display & system'),
]

# name -> (type, default expression)
VARIABLES = {
    # every page
    'sys_status': ('string', '""'),
    # home
    'home_info': ('string', '"..."'),
    'home_live': ('string', '"..."'),
    'home_services': ('string', '"..."'),
    # wifi
    'wifi_list': ('string', '"(press Scan)"'),
    'wifi_sel': ('integer', '0'),
    'wifi_pass': ('string', '""'),
    'wifi_scan_text': ('string', '""'),
    'wifi_status': ('string', '"..."'),
    'wifi_autoconnect': ('boolean', 'true'),
    # ble
    'ble_list': ('string', '"(press Scan)"'),
    'ble_sel': ('integer', '0'),
    'ble_scan_text': ('string', '""'),
    'ble_status': ('string', '"..."'),
    'ble_services': ('string', '""'),
    # can
    'can_bitrate_sel': ('integer', '2'),
    'can_listen_only': ('boolean', 'false'),
    'can_autostart': ('boolean', 'false'),
    'can_running': ('boolean', 'false'),
    'can_logging': ('boolean', 'false'),
    'can_run_text': ('string', '"Start"'),
    'can_log_text': ('string', '"Record"'),
    'can_stats': ('string', '"stopped"'),
    'can_log_info': ('string', '""'),
    'can_ids': ('string', '""'),
    'can_load': ('integer', '0'),
    # io
    'io_mode_sel': ('integer', '0'),
    'io_is_adc': ('boolean', 'true'),
    'io_is_out': ('boolean', 'false'),
    'io_value_text': ('string', '"-"'),
    'io_detail_text': ('string', '""'),
    'io_adc_mv': ('integer', '0'),
    'io_out_text': ('string', '"Set HIGH"'),
    'io_chip_text': ('string', '""'),
    'i2c_text': ('string', '"not scanned yet"'),
    # gps
    'gps_enabled': ('boolean', 'false'),
    'gps_fix_text': ('string', '"GPS off"'),
    'gps_pos_text': ('string', '""'),
    'gps_info_text': ('string', '""'),
    'gps_sats_text': ('string', '""'),
    'gps_link_text': ('string', '""'),
    'gps_raw_text': ('string', '""'),
    # route
    'gpx_list': ('string', '"(no files)"'),
    'gpx_sel': ('integer', '0'),
    'gpx_info': ('string', '""'),
    'route_big': ('string', '"-"'),
    'route_text': ('string', '""'),
    'route_view_text': ('string', '"Follow"'),
    # profile
    'profile_summary': ('string', '""'),
    'profile_live': ('string', '""'),
    'profile_climbs': ('string', '""'),
    'profile_view_text': ('string', '"Next 6 km"'),
    # ride
    'ride_speed': ('string', '"-"'),
    'ride_dist': ('string', '"-"'),
    'ride_moving': ('string', '"-"'),
    'ride_avg': ('string', '"-"'),
    'ride_max': ('string', '"-"'),
    'ride_climb': ('string', '"-"'),
    'ride_info': ('string', '""'),
    'ride_btn_text': ('string', '"Start ride"'),
    'ride_turn_caption': ('string', '"Next turn"'),
    'ride_turn_dist': ('string', '"-"'),
    # display
    'disp_brightness': ('integer', '100'),
    'disp_brightness_text': ('string', '"Brightness 100 %"'),
    'disp_timeout_sel': ('integer', '0'),
    'sd_text': ('string', '"..."'),
    'sys_version': ('string', '""'),
}

IO_MODES = ['Analog in (ADC)', 'Digital in', 'Digital in, pull-up', 'Digital in, pull-down', 'Digital out',
            'GPS (NMEA in)']      # order = io_mode_t

ACTIONS = [
    'nav_home', 'nav_wifi', 'nav_ble', 'nav_can', 'nav_io', 'nav_gps', 'nav_route', 'nav_profile',
    'nav_ride', 'nav_display',
    'wifi_scan', 'wifi_connect', 'wifi_disconnect', 'wifi_forget',
    'ble_scan', 'ble_connect', 'ble_disconnect', 'ble_forget',
    'can_start_stop', 'can_log_toggle', 'can_reset',
    'io_toggle_out', 'io_reset', 'i2c_scan',
    'gps_enable',
    'profile_view',
    'ride_start', 'ride_stop',
    'gpx_load', 'gpx_reverse', 'gpx_rescan', 'route_zoom_in', 'route_zoom_out', 'route_view',
    'sd_remount', 'screen_off', 'restart',
]


def uid():
    return str(uuid.uuid4())


def fresh_ids(o):
    if isinstance(o, dict):
        if 'objID' in o:
            o['objID'] = uid()
        for v in o.values():
            fresh_ids(v)
    elif isinstance(o, list):
        for v in o:
            fresh_ids(v)
    return o


class B:
    """Widget builder on top of the EEZ templates."""

    def __init__(self, tpl):
        self.tpl = tpl['widgets']

    def base(self, wtype, x, y, w, h, styles):
        t = copy.deepcopy(self.tpl[wtype])
        t.update(left=x, top=y, width=w, height=h, leftUnit='px', topUnit='px',
                 widthUnit='px', heightUnit='px', children=[], eventHandlers=[],
                 hiddenFlagType='literal', group='', groupIndex=0)
        t.pop('hiddenFlag', None)
        t['localStyles'] = {'objID': uid(), 'definition': styles}
        t['style'] = {'objID': uid(), 'useStyle': 'default', 'conditionalStyles': [], 'childStyles': []}
        return fresh_ids(t)

    @staticmethod
    def hide(t, expr):
        if expr:
            t['hiddenFlagType'] = 'expression'
            t['hiddenFlag'] = expr
        return t

    @staticmethod
    def on_click(t, action):
        t['eventHandlers'].append({'objID': uid(), 'eventName': 'CLICKED', 'handlerType': 'action',
                                   'action': action, 'userData': 0})
        return t

    def label(self, text, x, y, w=None, font=16, color=WHITE, expr=False, wrap=False, align=None, hidden=None):
        st = {'text_color': color, 'text_font': f'MONTSERRAT_{font}' if isinstance(font, int) else font}
        if align:
            st['text_align'] = align
        t = self.base('LVGLLabelWidget', x, y, w or 0, 30, {'MAIN': {'DEFAULT': st}})
        t['widthUnit'] = 'px' if w else 'content'
        t['heightUnit'] = 'content'
        t.update(text=text, textType='expression' if expr else 'literal',
                 longMode='WRAP' if wrap else 'CLIP', recolor=False)
        return self.hide(t, hidden)

    def panel(self, x, y, w, h, children=(), bg=PANEL, scroll=False, radius=8, pad=12, hidden=None, border=True):
        st = {'pad_top': pad, 'pad_bottom': pad, 'pad_left': pad, 'pad_right': pad,
              'bg_color': bg, 'bg_opa': 255, 'radius': radius,
              'border_width': 1 if border else 0, 'border_color': BORDER}
        t = self.base('LVGLPanelWidget', x, y, w, h, {'MAIN': {'DEFAULT': st}})
        if scroll:
            t['widgetFlags'] = 'GESTURE_BUBBLE|PRESS_LOCK|SCROLLABLE|SCROLL_ELASTIC|SCROLL_MOMENTUM|SNAPPABLE'
            t['flagScrollDirection'] = 'ver'
            t['flagScrollbarMode'] = 'auto'
        t['children'] = list(children)
        return self.hide(t, hidden)

    def button(self, text, x, y, w, h, action, color=BTN, font=20, expr=False, hidden=None):
        t = self.base('LVGLButtonWidget', x, y, w, h, {'MAIN': {'DEFAULT': {
            'bg_color': color, 'radius': 8, 'shadow_width': 0}}})
        lbl = self.label(text, 0, 0, None, font, WHITE, expr=expr, align='CENTER')
        lbl['localStyles']['definition']['MAIN']['DEFAULT'].update(align='CENTER')
        t['children'] = [lbl]
        self.on_click(t, action)
        return self.hide(t, hidden)

    def checkbox(self, text, var, x, y, font=20):
        t = self.base('LVGLCheckboxWidget', x, y, 200, 32, {'MAIN': {'DEFAULT': {
            'text_color': WHITE, 'text_font': f'MONTSERRAT_{font}'}}})
        t.update(widthUnit='content', heightUnit='content', text=text, textType='literal',
                 checkedStateType='expression', checkedState=var)
        return t

    def dropdown(self, options_var, sel_var, x, y, w, literal_options=None, font=20):
        t = self.base('LVGLDropdownWidget', x, y, w, 44, {'MAIN': {'DEFAULT': {
            'text_font': f'MONTSERRAT_{font}', 'bg_color': BTN, 'text_color': WHITE,
            'border_width': 1, 'border_color': BORDER}}})
        t['heightUnit'] = 'content'
        if literal_options is not None:
            t.update(options='\n'.join(literal_options), optionsType='literal')
        else:
            t.update(options=options_var, optionsType='expression')
        t.update(selected=sel_var, selectedType='expression', direction='bottom')
        return t

    def textarea(self, var, x, y, w, placeholder='', password=False, maxlen=64):
        t = self.base('LVGLTextareaWidget', x, y, w, 44, {'MAIN': {'DEFAULT': {
            'text_font': 'MONTSERRAT_20', 'bg_color': BTN, 'text_color': WHITE,
            'border_width': 1, 'border_color': BORDER}}})
        t.update(text=var, textType='expression', placeholder=placeholder, oneLineMode=True,
                 passwordMode=password, acceptedCharacters='', maxTextLength=maxlen)
        return t

    def slider(self, var, vmin, vmax, x, y, w):
        t = self.base('LVGLSliderWidget', x, y, w, 16, {
            'MAIN': {'DEFAULT': {'bg_color': BTN}},
            'INDICATOR': {'DEFAULT': {'bg_color': ACCENT}},
            'KNOB': {'DEFAULT': {'bg_color': WHITE}}})
        t.update(min=vmin, minType='literal', max=vmax, maxType='literal', mode='NORMAL',
                 value=var, valueType='expression', previewValue=vmax, valueLeft=0,
                 valueLeftType='literal', previewValueLeft=0, enableAnimation=False)
        return t

    def bar(self, var, vmin, vmax, x, y, w, h, color=ACCENT, hidden=None):
        t = self.base('LVGLBarWidget', x, y, w, h, {
            'MAIN': {'DEFAULT': {'bg_color': BTN, 'radius': 4}},
            'INDICATOR': {'DEFAULT': {'bg_color': color, 'radius': 4}}})
        t.update(min=vmin, minType='literal', max=vmax, maxType='literal', mode='NORMAL',
                 value=var, valueType='expression', previewValue=(vmin + vmax) // 2,
                 valueLeft=0, valueLeftType='literal', previewValueLeft=0, enableAnimation=False)
        return self.hide(t, hidden)


# ------------------------------------------------------------------ pages

def ident(t, name):
    """EEZ identifier -> objects.<name> in screens.h. Only for what bindings
    can't do: the CAN ID table gets LVGL's monospace font from the bridge
    (EEZ 0.29 only offers the Montserrat built-in fonts)."""
    t['identifier'] = name
    return t


def nav_label(text):
    """one line: symbol and name (10 buttons don't leave room for two)"""
    return text.replace('\n', '  ')


def nav_button(b, name, text, i, color=SIDEBAR):
    return b.button(nav_label(text), 5, 8 + i * NAV_STEP, SIDEBAR_W - 10, NAV_H, f'nav_{name}', color=color,
                    font=16)


def chrome(b, page_name, title):
    """Sidebar with the page buttons, title and status line."""
    items = []
    btns = []
    for i, (name, text, _) in enumerate(PAGES):
        color = ACCENT if name == page_name else SIDEBAR
        btns.append(nav_button(b, name, text, i, color))
    items.append(b.panel(0, 0, SIDEBAR_W, 480, btns, bg=SIDEBAR, radius=0, pad=0, border=False))
    items.append(b.label(title, CX, 8, None, 24))
    items.append(b.label('sys_status', CX + 230, 12, CW - 230, 16, GREY, expr=True, align='RIGHT'))
    return items


def page_home(b):
    w2 = (CW - 10) // 2
    return [
        b.panel(CX, TOP, w2, 250, [b.label('home_info', 0, 0, w2 - 26, 16, expr=True, wrap=True)]),
        b.panel(CX + w2 + 10, TOP, w2, 250, [b.label('home_live', 0, 0, w2 - 26, 16, expr=True, wrap=True)]),
        b.panel(CX, TOP + 260, CW, 164, [
            b.label('Services', 0, 0, None, 16, GREY),
            b.label('home_services', 0, 26, CW - 26, 16, expr=True, wrap=True)]),
    ]


def page_wifi(b):
    return [
        b.panel(CX, TOP, CW, 200, [
            b.button('Scan', 0, 0, 120, 44, 'wifi_scan', ACCENT),
            b.label('wifi_scan_text', 135, 12, 480, 16, GREY, expr=True),
            b.label('Network', 0, 64, None, 16, GREY),
            b.dropdown('wifi_list', 'wifi_sel', 100, 54, 520),
            b.label('Password', 0, 122, None, 16, GREY),
            b.textarea('wifi_pass', 100, 112, 520, 'password (empty = open network)', password=True),
        ]),
        b.panel(CX, TOP + 210, 400, 214, [
            b.label('wifi_status', 0, 0, 374, 16, expr=True, wrap=True)], scroll=True),
        b.panel(CX + 410, TOP + 210, CW - 410, 214, [
            b.button('Connect', 0, 0, 216, 44, 'wifi_connect', ACCENT),
            b.button('Disconnect', 0, 54, 216, 44, 'wifi_disconnect'),
            b.button('Forget', 0, 108, 216, 44, 'wifi_forget', DANGER),
            b.checkbox('Connect at boot', 'wifi_autoconnect', 0, 160, 16),
        ]),
    ]


def page_ble(b):
    return [
        b.panel(CX, TOP, CW, 130, [
            b.button('Scan 10 s', 0, 0, 140, 44, 'ble_scan', ACCENT),
            b.label('ble_scan_text', 155, 12, 460, 16, GREY, expr=True),
            b.label('Device', 0, 64, None, 16, GREY),
            b.dropdown('ble_list', 'ble_sel', 100, 54, 520, font=16),
        ]),
        b.panel(CX, TOP + 140, 300, 284, [
            b.label('ble_status', 0, 0, 274, 16, expr=True, wrap=True)], scroll=True),
        b.panel(CX + 310, TOP + 140, 160, 284, [
            b.button('Connect\n+ pair', 0, 0, 134, 70, 'ble_connect', ACCENT, font=16),
            b.button('Disconnect', 0, 80, 134, 50, 'ble_disconnect', font=16),
            b.button('Forget\nbonds', 0, 140, 134, 70, 'ble_forget', DANGER, font=16),
        ]),
        b.panel(CX + 480, TOP + 140, CW - 480, 284, [
            b.label('GATT services', 0, 0, None, 16, GREY),
            b.label('ble_services', 0, 26, CW - 506, 'MONTSERRAT_12', expr=True, wrap=True)], scroll=True),
    ]


def page_can(b):
    bitrates = ['125 kbit/s', '250 kbit/s', '500 kbit/s', '1 Mbit/s']
    return [
        b.panel(CX, TOP, 250, 424, [
            b.label('Bitrate', 0, 0, None, 16, GREY),
            b.dropdown(None, 'can_bitrate_sel', 0, 24, 224, literal_options=bitrates),
            b.checkbox('Listen only', 'can_listen_only', 0, 80, 16),
            b.checkbox('Start at boot', 'can_autostart', 0, 112, 16),
            b.button('can_run_text', 0, 152, 224, 50, 'can_start_stop', ACCENT, expr=True),
            b.button('can_log_text', 0, 212, 224, 50, 'can_log_toggle', DANGER, expr=True),
            b.button('Clear stats', 0, 272, 224, 40, 'can_reset', font=16),
            b.label('can_log_info', 0, 322, 224, 'MONTSERRAT_12', GREY, expr=True, wrap=True),
        ]),
        b.panel(CX + 260, TOP, CW - 260, 130, [
            b.label('can_stats', 0, 0, 230, 'MONTSERRAT_12', expr=True, wrap=True),
            b.label('Bus load', 250, 0, None, 12, GREY),
            b.bar('can_load', 0, 100, 250, 20, CW - 260 - 276, 18),
        ]),
        b.panel(CX + 260, TOP + 140, CW - 260, 284, [
            ident(b.label('can_ids', 0, 0, CW - 260 - 26, 'MONTSERRAT_12', expr=True), 'can_id_table')],
                scroll=True, pad=8),
    ]


def page_io(b):
    modes = IO_MODES
    return [
        b.panel(CX, TOP, CW, 250, [
            b.label('Sensor AD header (J8)  -  GPIO6, 0..3.3 V', 0, 0, None, 16, GREY),
            b.label('Mode', 0, 42, None, 16, GREY),
            b.dropdown(None, 'io_mode_sel', 60, 32, 300, literal_options=modes),
            b.label('io_value_text', 380, 26, 250, 48, expr=True),
            b.bar('io_adc_mv', 0, 3300, 0, 100, CW - 26, 24, GREEN, hidden='!io_is_adc'),
            b.label('io_detail_text', 0, 140, 420, 16, GREY, expr=True, wrap=True),
            b.button('io_out_text', 440, 130, 180, 50, 'io_toggle_out', ACCENT, expr=True, hidden='!io_is_out'),
            b.button('Reset min/max', 440, 190, 180, 36, 'io_reset', font=16),
        ]),
        b.panel(CX, TOP + 260, CW, 164, [
            b.label('io_chip_text', 0, 0, CW - 26, 16, expr=True),
            b.label('I2C header (shared with touch + IO expander)', 0, 40, None, 16, GREY),
            b.button('Scan I2C', 0, 70, 140, 44, 'i2c_scan', ACCENT),
            b.label('i2c_text', 155, 80, CW - 26 - 155, 16, expr=True, wrap=True),
        ]),
    ]


def page_gps(b):
    """gps_sat_table / gps_raw_table get LVGL's monospace fonts from the bridge."""
    wl = 330
    return [
        b.panel(CX, TOP, wl, 270, [
            b.label('gps_fix_text', 0, 0, wl - 26, 24, expr=True),
            b.label('gps_pos_text', 0, 36, wl - 26, 20, expr=True, wrap=True),
            b.label('gps_info_text', 0, 96, wl - 26, 16, GREY, expr=True, wrap=True),
        ]),
        b.panel(CX + wl + 10, TOP, CW - wl - 10, 270, [
            b.label('Satellites   (* = used, SNR dB-Hz)', 0, 0, None, 12, GREY),
            ident(b.label('gps_sats_text', 0, 22, CW - wl - 36, 16, expr=True), 'gps_sat_table'),
        ], scroll=True),
        b.panel(CX, TOP + 280, CW, 144, [
            b.label('gps_link_text', 0, 0, CW - 26 - 230, 16, expr=True, wrap=True),
            b.button('Use GPIO6 for GPS', CW - 26 - 220, 0, 220, 44, 'gps_enable', ACCENT, font=16,
                     hidden='gps_enabled'),
            ident(b.label('gps_raw_text', 0, 56, CW - 26, 12, GREY, expr=True, wrap=True), 'gps_raw_table'),
        ], pad=10),
    ]


def page_route(b):
    """route_map is an empty panel: the bridge draws the track, position and
    scale into it (lv_line / lv_obj children)."""
    wm = 430
    wr = CW - wm - 10
    iw = wr - 20
    return [
        ident(b.panel(CX, TOP, wm, 424, [], bg='#0e141b', pad=0), 'route_map'),
        b.panel(CX + wm + 10, TOP, wr, 424, [
            b.label('GPX file  (SD: /gpx)', 0, 0, None, 12, GREY),
            b.dropdown('gpx_list', 'gpx_sel', 0, 18, iw, font=16),
            b.button('Load', 0, 66, (iw - 12) // 3, 40, 'gpx_load', ACCENT, font=12),
            b.button('Reverse', (iw - 12) // 3 + 6, 66, (iw - 12) // 3, 40, 'gpx_reverse', font=12),
            b.button('Rescan', 2 * ((iw - 12) // 3 + 6), 66, (iw - 12) // 3, 40, 'gpx_rescan', font=12),
            b.label('gpx_info', 0, 116, iw, 12, GREY, expr=True, wrap=True),
            b.label('route_big', 0, 196, iw, 24, expr=True),     # one line, cut off when too long
            b.label('route_text', 0, 234, iw, 16, expr=True, wrap=True),
            b.button('-', 0, 350, 56, 48, 'route_zoom_out', font=24),
            b.button('+', 62, 350, 56, 48, 'route_zoom_in', font=24),
            b.button('route_view_text', 124, 350, iw - 124, 48, 'route_view', font=16, expr=True),
        ], pad=10),
    ]


def page_profile(b):
    """profile_chart is an empty panel the bridge draws the elevation profile
    into; profile_climb_table gets a monospace font from the bridge."""
    wl = 270
    return [
        ident(b.panel(CX, TOP, CW, 214, [], bg='#0e141b', pad=0), 'profile_chart'),
        b.panel(CX, TOP + 224, wl, 200, [
            b.label('profile_summary', 0, 0, wl - 24, 12, GREY, expr=True, wrap=True),
            b.label('profile_live', 0, 36, wl - 24, 16, expr=True, wrap=True),
            b.button('profile_view_text', 0, 134, wl - 24, 42, 'profile_view', font=16, expr=True),
        ]),
        b.panel(CX + wl + 10, TOP + 224, CW - wl - 10, 200, [
            ident(b.label('profile_climbs', 0, 0, CW - wl - 36, 16, expr=True), 'profile_climb_table'),
        ], scroll=True, pad=8),
    ]


def page_ride(b):
    """Big figures for riding. Bottom row: the navigation arrow (ride_turn_arrow,
    an empty panel the bridge draws into), the distance to the next turn and
    the ride info with the button. The button starts on a short click and
    stops only on a long press (no accidental stop)."""
    tw = (CW - 20) // 3
    th = 118
    tiles = [('Speed  km/h', 'ride_speed'), ('Distance  km', 'ride_dist'), ('Moving time', 'ride_moving'),
             ('Average  km/h', 'ride_avg'), ('Top speed  km/h', 'ride_max'), ('Climbed  m (GPS)', 'ride_climb')]
    out = []
    for i, (cap, var) in enumerate(tiles):
        x = CX + (i % 3) * (tw + 10)
        y = TOP + (i // 3) * (th + 10)
        out.append(b.panel(x, y, tw, th, [
            b.label(cap, 0, 0, None, 12, GREY),
            b.label(var, 0, 22, tw - 24, 48, expr=True, align='CENTER'),
        ]))
    y3 = TOP + 2 * (th + 10)
    h3 = 424 - 2 * (th + 10)
    wa, wd = 150, 200
    wi = CW - wa - wd - 20
    out.append(ident(b.panel(CX, y3, wa, h3, [], pad=0), 'ride_turn_arrow'))
    out.append(b.panel(CX + wa + 10, y3, wd, h3, [
        b.label('ride_turn_caption', 0, 0, wd - 24, 12, GREY, expr=True, wrap=True),
        b.label('ride_turn_dist', 0, 50, wd - 24, 48, expr=True, align='CENTER'),
    ]))
    btn = b.button('ride_btn_text', 0, h3 - 24 - 50, wi - 24, 50, 'ride_start', ACCENT, expr=True)
    for h in btn['eventHandlers']:
        h['eventName'] = 'SHORT_CLICKED'
    btn['eventHandlers'].append({'objID': uid(), 'eventName': 'LONG_PRESSED', 'handlerType': 'action',
                                 'action': 'ride_stop', 'userData': 0})
    out.append(b.panel(CX + wa + wd + 20, y3, wi, h3, [
        b.label('ride_info', 0, 0, wi - 24, 12, expr=True, wrap=True),
        btn,
    ]))
    return out


# extra variables / actions of pages added after the first scaffold
# (tools/eez_add_page.py): page -> (variable name prefixes, actions)
PAGE_EXTRAS = {
    'gps': (('gps_',), ('nav_gps', 'gps_enable')),
    'route': (('gpx_', 'route_'), ('nav_route', 'gpx_load', 'gpx_reverse', 'gpx_rescan', 'route_zoom_in',
                                   'route_zoom_out', 'route_view')),
    'profile': (('profile_',), ('nav_profile', 'profile_view')),
    'ride': (('ride_',), ('nav_ride', 'ride_start', 'ride_stop')),
}


def page_display(b):
    timeouts = ['Never', '30 s', '1 min', '5 min', '10 min']
    return [
        b.panel(CX, TOP, CW, 190, [
            b.label('disp_brightness_text', 0, 0, None, 20, expr=True),
            b.slider('disp_brightness', 10, 100, 10, 40, CW - 50),
            b.label('Backlight is on/off only on this board: brightness dims the picture (software).',
                    0, 66, CW - 26, 12, GREY, wrap=True),
            b.label('Screen off after', 0, 106, None, 16, GREY),
            b.dropdown(None, 'disp_timeout_sel', 150, 96, 180, literal_options=timeouts),
            b.button('Screen off now', 350, 96, 200, 44, 'screen_off', font=16),
        ]),
        b.panel(CX, TOP + 200, CW, 224, [
            b.label('sd_text', 0, 0, CW - 26 - 200, 16, expr=True, wrap=True),
            b.button('Remount SD', CW - 26 - 180, 0, 180, 44, 'sd_remount', font=16),
            b.label('sys_version', 0, 120, CW - 26 - 200, 12, GREY, expr=True, wrap=True),
            b.button('Restart', CW - 26 - 180, 150, 180, 44, 'restart', DANGER, font=16),
        ]),
    ]


BUILDERS = {
    'home': page_home, 'wifi': page_wifi, 'ble': page_ble,
    'can': page_can, 'io': page_io, 'gps': page_gps, 'route': page_route, 'profile': page_profile, 'ride': page_ride,
    'display': page_display,
}


def make_page(tpl, b, name, title):
    page = copy.deepcopy(tpl['page'])
    fresh_ids(page)
    page.update(name=name, createAtStart=True, deleteOnScreenUnload=False)
    scr = b.base('LVGLScreenWidget', 0, 0, 800, 480, {'MAIN': {'DEFAULT': {'bg_color': BG}}})
    scr['widgetFlags'] = 'CLICKABLE|PRESS_LOCK|CLICK_FOCUSABLE|GESTURE_BUBBLE|SNAPPABLE'
    scr['children'] = chrome(b, name, title) + BUILDERS[name](b)
    page['components'] = [scr]
    return page


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--force', action='store_true', help='overwrite an existing project')
    args = ap.parse_args()
    if os.path.exists(PROJECT) and not args.force:
        sys.exit(f'{PROJECT} exists; it is edited in EEZ Studio now. Use --force to replace it.')

    with open(TEMPLATES, encoding='utf-8') as f:
        tpl = json.load(f)
    b = B(tpl)
    proj = fresh_ids(copy.deepcopy(tpl['project']))
    proj['settings']['general']['darkTheme'] = True     # lv_theme_default_init(..., dark)
    proj['userPages'] = [make_page(tpl, b, n, t) for n, _, t in PAGES]
    proj['variables']['globalVariables'] = [
        {'objID': uid(), 'name': n, 'type': t, 'defaultValue': d, 'persistent': False}
        for n, (t, d) in VARIABLES.items()]
    proj['actions'] = [
        {'objID': uid(), 'components': [], 'connectionLines': [], 'localVariables': [],
         'userProperties': [], 'name': a, 'implementationType': 'native'} for a in ACTIONS]

    os.makedirs(os.path.dirname(PROJECT), exist_ok=True)
    with open(PROJECT, 'w', encoding='utf-8') as f:
        json.dump(proj, f, indent=2, ensure_ascii=False)
    print(f'wrote {os.path.relpath(PROJECT)}: {len(PAGES)} screens, {len(VARIABLES)} variables, {len(ACTIONS)} actions')


if __name__ == '__main__':
    main()

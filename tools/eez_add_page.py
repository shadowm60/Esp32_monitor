#!/usr/bin/env python3
"""
Adds one page to the existing EEZ project in place (keeps every change made
in EEZ Studio). The page must be defined in eez_scaffold.py (PAGES, BUILDERS,
VARIABLES, ACTIONS, PAGE_EXTRAS):

  - its global variables (PAGE_EXTRAS prefixes) and native actions
  - a sidebar button on every page, in PAGES order (buttons re-spaced)
  - the screen itself, before the next page in PAGES order

Does nothing if a screen with that name exists.

    python3 tools/eez_add_page.py route && tools/eez-build.sh

(The "gps" page also added "GPS (NMEA in)" to the IO mode list.)
"""
import argparse
import json
import sys

import eez_scaffold as sc


def nav_action(w):
    for h in w.get('eventHandlers', []):
        if h.get('action', '').startswith('nav_'):
            return h['action']
    return None


def find_sidebar(screen):
    for c in screen['children']:
        if c['type'] == 'LVGLPanelWidget' and any(nav_action(k) for k in c['children']):
            return c
    return None


def walk(w):
    yield w
    for c in w.get('children', []):
        yield from walk(c)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('page', choices=sorted(sc.PAGE_EXTRAS))
    page_name = ap.parse_args().page
    prefixes, actions = sc.PAGE_EXTRAS[page_name]
    title = next(t for n, _, t in sc.PAGES if n == page_name)
    text = next(x for n, x, _ in sc.PAGES if n == page_name)

    with open(sc.PROJECT, encoding='utf-8') as f:
        proj = json.load(f)
    if any(p['name'] == page_name for p in proj['userPages']):
        sys.exit(f'project already has a {page_name} screen, nothing to do')
    with open(sc.TEMPLATES, encoding='utf-8') as f:
        tpl = json.load(f)
    b = sc.B(tpl)

    gvars = proj['variables']['globalVariables']
    have = {v['name'] for v in gvars}
    for n, (t, d) in sc.VARIABLES.items():
        if n.startswith(prefixes) and n not in have:
            gvars.append({'objID': sc.uid(), 'name': n, 'type': t, 'defaultValue': d, 'persistent': False})
    have = {a['name'] for a in proj['actions']}
    for a in actions:
        if a not in have:
            proj['actions'].append({'objID': sc.uid(), 'components': [], 'connectionLines': [],
                                    'localVariables': [], 'userProperties': [], 'name': a,
                                    'implementationType': 'native'})

    # sidebars: add the button in PAGES order, re-space
    order = [n for n, _, _ in sc.PAGES]
    later = order[order.index(page_name) + 1:]
    for p in proj['userPages']:
        bar = find_sidebar(p['components'][0])
        if bar is None:
            print(f'warning: no sidebar found on {p["name"]}, add the {page_name} button by hand')
            continue
        btns = [k for k in bar['children'] if nav_action(k)]
        if not any(nav_action(k) == f'nav_{page_name}' for k in btns):
            btn = sc.nav_button(b, page_name, text, 0)
            idx = next((i for i, k in enumerate(bar['children']) if (nav_action(k) or '')[4:] in later),
                       len(bar['children']))
            bar['children'].insert(idx, btn)
            btns = [k for k in bar['children'] if nav_action(k)]
        btns.sort(key=lambda k: order.index(nav_action(k)[4:]) if nav_action(k)[4:] in order else 99)
        for i, k in enumerate(btns):
            k['top'] = 8 + i * sc.NAV_STEP
            k['height'] = sc.NAV_H
            for lbl in k['children']:
                if lbl.get('type') == 'LVGLLabelWidget' and lbl.get('textType') == 'literal':
                    lbl['text'] = sc.nav_label(lbl['text'])

    page = sc.make_page(tpl, b, page_name, title)
    idx = next((i for i, p in enumerate(proj['userPages']) if p['name'] in later), len(proj['userPages']))
    proj['userPages'].insert(idx, page)

    with open(sc.PROJECT, 'w', encoding='utf-8') as f:
        json.dump(proj, f, indent=2, ensure_ascii=False)
    print(f'added the {page_name} screen: {len(proj["userPages"])} screens, {len(gvars)} variables, '
          f'{len(proj["actions"])} actions')


if __name__ == '__main__':
    main()

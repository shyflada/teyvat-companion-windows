// Adapter to the official map's existing marker UI. No HTTP calls, cookies,
// account data, or guessed endpoints. A changed site fails with no local ack.
(request) => {
    let visibleIds;
    const result = (state, extra = {}) => ({ state, ...(visibleIds ? { ids: visibleIds } : {}), ...extra });
    const allowedMaps = [2, 7, 9, 36], chestTypes = [17, 44, 45, 46, 269];
    if (location.protocol !== 'https:' || location.hostname !== 'act.hoyolab.com' ||
        location.pathname !== '/ys/app/interactive-map/index.html') return result('wrong_site');
    if (!allowedMaps.includes(request.map)) return result('invalid_request');
    const currentMap = Number(location.hash.match(/^#\/map\/(\d+)(?:[/?]|$)/)?.[1]);
    if (currentMap !== request.map) return result('wrong_map');
    let element = document.querySelector('.leaflet-container'), gis;
    while (element && !gis) {
        for (let v = element.__vue__; v; v = v.$parent) {
            if (v.$options?.name === 'MhyGameGis') { gis = v; break; }
        }
        element = element.parentElement;
    }
    const store = gis?.$store;
    if (!store || !Array.isArray(store.state.markPointList) || !Array.isArray(gis.markerList)) return result('loading');
    if (!store.getters.loggedIn) return result('login_required');
    const pins = store.state.markPointList;
    const ids = [...new Set(pins.filter(p => chestTypes.includes(Number(p.label_id)))
        .map(p => Number(p.point_id)).filter(p => Number.isSafeInteger(p) && p > 0))];
    visibleIds = ids;
    if (!request.write) return result('ready', { ids });
    const id = request.point;
    if (!Number.isSafeInteger(id) || id <= 0 || typeof request.collected !== 'boolean') return result('invalid_request');
    const marked = pins.some(p => Number(p.point_id) === id);
    if (request.collected && pins.some(p => Number(p.point_id) === id && chestTypes.includes(Number(p.label_id)))) {
        delete window.__teyvatPinWrite; return result('ack', { point: id, collected: true, ids });
    }
    const raw = gis.markerList.find(p => Number(p.id) === id && chestTypes.includes(Number(p.label_id)));
    if (!raw) return result('pin_unavailable');
    if (marked === request.collected) { delete window.__teyvatPinWrite; return result('ack', { point: id, collected: marked, ids }); }
    if (!raw.marker) return result('pin_unavailable');
    const pending = window.__teyvatPinWrite;
    if (pending) {
        if (pending.map !== request.map || pending.point !== id || pending.collected !== request.collected) return result('busy');
        if (Date.now() - pending.at < 15000) return result('pending');
        delete window.__teyvatPinWrite;
        return result('not_acknowledged');
    }
    const marker = raw.marker;
    if (typeof marker.openPopup !== 'function' || typeof marker.getPopup !== 'function' ||
        typeof marker.isPopupOpen !== 'function' || typeof marker.getLatLng !== 'function') return result('site_changed');
    if (!marker.isPopupOpen()) {
        const map = gis.map;
        if (!map?.panTo) return result('pin_unavailable');
        const position = marker.getLatLng();
        if (!marker._map) {
            let cluster;
            map.eachLayer?.(layer => {
                if (!cluster && typeof layer.zoomToShowLayer === 'function' && layer.hasLayer?.(marker)) cluster = layer;
            });
            if (cluster) {
                cluster.zoomToShowLayer(marker, () => {});
            } else if (typeof map.setView === 'function' && typeof map.getZoom === 'function') {
                map.setView(position, Math.max(map.getZoom(), 0), { animate: false });
            } else {
                return result('pin_unavailable');
            }
            return result('selecting');
        }
        map.panTo(position);
        marker.openPopup();
        return result('selecting');
    }
    // On mobile the Leaflet popup is empty; HoYoLAB renders the card elsewhere.
    const mobileCard = [...document.querySelectorAll('.map-popup')].find(card => {
        const component = card.__vue__;
        return card.getClientRects().length > 0 && Number(component?.marker?.id) === id
            && Number(component.mapId) === request.map;
    });
    const root = mobileCard ?? marker.getPopup()?.getElement();
    const vue = mobileCard ? mobileCard.__vue__
        : root && [root, ...root.querySelectorAll('*')].find(e => e.vueInstance)?.vueInstance;
    if (!vue) return result('selecting');
    if (Number(vue.marker?.id) !== id || Number(vue.mapId) !== request.map) return result('wrong_pin');
    // The official handler changes marked and markPointList after its request
    // succeeds. Never acknowledge a click or an optimistic local queue entry.
    if (Boolean(vue.marked) !== marked) return result('pending');
    const label = request.collected ? vue.$MI18N?.WORD?.mark : vue.$MI18N?.WORD?.unmark;
    const normalize = s => String(s ?? '').replace(/\s+/g, ' ').trim();
    const buttons = [...root.querySelectorAll('.map-popup__btn')]
        .filter(b => !b.classList.contains('map-popup__btn--batch') && normalize(b.innerText) === normalize(label));
    if (!label || buttons.length !== 1 || buttons[0].disabled || buttons[0].getAttribute('aria-disabled') === 'true') return result('site_changed');
    window.__teyvatPinWrite = { map: request.map, point: id, collected: request.collected, at: Date.now() };
    buttons[0].click();
    return result('pending');
}

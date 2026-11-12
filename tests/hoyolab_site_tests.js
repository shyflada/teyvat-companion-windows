// Run with Node's standard library; no browser or owner account is involved.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname, '../src/hoyolab_site.js'), 'utf8');
let clicks = 0;
const pins = [];
const popup = { marker: { id: 724 }, mapId: 2, marked: false, $MI18N: { WORD: { mark: 'Mark Pin', unmark: 'Unmark Pin' } } };
const button = { innerText: 'Mark Pin', classList: { contains: () => false }, getAttribute: () => null, click: () => ++clicks };
const root = { vueInstance: popup, querySelectorAll: selector => selector === '*' ? [] : [button] };
let open = true;
let leafletPopup = root;
let mobileCards = [];
const marker = { isPopupOpen: () => open, openPopup: () => { open = true; }, getPopup: () => ({ getElement: () => leafletPopup }), _map: {}, getLatLng: () => ({ lat: 7, lng: 346 }) };
const store = { getters: { loggedIn: true }, state: { markPointList: pins } };
const gis = { $options: { name: 'MhyGameGis' }, $store: store, markerList: [{ id: 724, label_id: 17, marker }], map: { panTo: () => {} } };
const context = vm.createContext({ location: { protocol: 'https:', hostname: 'act.hoyolab.com', pathname: '/ys/app/interactive-map/index.html', hash: '#/map/2?shown_types=17' }, document: { querySelector: () => ({ __vue__: gis }), querySelectorAll: () => mobileCards }, window: {}, Date });
const adapter = vm.runInContext(`(${source})`, context);
const request = { map: 2, point: 724, collected: true, write: true };
assert.equal(adapter({ map: 2, write: false }).state, 'ready');
store.getters.loggedIn = false;
assert.equal(adapter(request).state, 'login_required'); assert.equal(clicks, 0);
store.getters.loggedIn = true;
assert.equal(adapter({ ...request, point: 725 }).state, 'pin_unavailable');
open = false;
marker._map = null;
gis.map.getZoom = () => -2;
gis.map.setView = (position, zoom, options) => {
    assert.deepEqual(position, { lat: 7, lng: 346 });
    assert.equal(zoom, 0);
    assert.equal(options.animate, false);
    marker._map = gis.map;
};
assert.equal(adapter(request).state, 'selecting');
assert.equal(open, false);
marker._map = null;
gis.map.eachLayer = visit => visit({
    hasLayer: layer => layer === marker,
    zoomToShowLayer: (layer, done) => {
        assert.equal(layer, marker);
        marker._map = gis.map;
        done();
    }
});
assert.equal(adapter(request).state, 'selecting');
assert.equal(open, false);
assert.equal(adapter(request).state, 'selecting');
assert.equal(open, true);
assert.equal(clicks, 0);
popup.marker.id = 725;
assert.equal(adapter(request).state, 'wrong_pin'); assert.equal(clicks, 0);
popup.marker.id = 724;
assert.equal(adapter(request).state, 'pending'); assert.equal(clicks, 1);
assert.equal(adapter(request).state, 'pending'); assert.equal(clicks, 1); // no ack from a click
pins.push({ label_id: 17, point_id: 724 }); popup.marked = true;
assert.equal(adapter(request).state, 'ack'); assert.equal(clicks, 1); // server-backed store changed
assert.equal(adapter(request).state, 'ack'); assert.equal(clicks, 1); // never toggle a collected pin
const savedMarker = gis.markerList[0].marker; gis.markerList[0].marker = undefined;
assert.equal(adapter(request).state, 'ack'); assert.equal(clicks, 1); // a hidden collected pin needs no popup
gis.markerList[0].marker = savedMarker;
button.innerText = 'Unmark Pin';
assert.equal(adapter({ ...request, collected: false }).state, 'pending'); assert.equal(clicks, 2);
assert.equal(adapter({ ...request, collected: false }).state, 'pending'); assert.equal(clicks, 2);
pins.length = 0; popup.marked = false;
assert.equal(adapter({ ...request, collected: false }).state, 'ack');
assert.equal(adapter({ ...request, point: -1 }).state, 'invalid_request');
assert.equal(adapter({ ...request, map: 7 }).state, 'wrong_map');
context.location.hostname = 'example.com';
assert.equal(adapter(request).state, 'wrong_site'); assert.equal(clicks, 2);

// Android's visible card is outside the empty Leaflet popup and uses __vue__.
context.location.hostname = 'act.hoyolab.com';
leafletPopup = { querySelectorAll: () => [] };
button.innerText = 'Mark Pin';
const card = (view, visible, control) => ({
    __vue__: view, getClientRects: () => visible ? [{}] : [], querySelectorAll: () => [control]
});
const wrongButton = { ...button, click: () => assert.fail('Clicked a different or hidden card') };
for (let id = 1000; id < 1100; ++id) {
    const view = { ...popup, marker: { id }, marked: false };
    const write = { ...request, point: id };
    gis.markerList[0].id = id;
    mobileCards = [
        card({ ...view, marker: { id: id - 1 } }, true, wrongButton),
        card({ ...view, mapId: 7 }, true, wrongButton),
        card(view, false, wrongButton)
    ];
    assert.equal(adapter(write).state, 'selecting');
    const before = clicks;
    mobileCards.push(card(view, true, button));
    assert.equal(adapter(write).state, 'pending');
    assert.equal(adapter(write).state, 'pending');
    assert.equal(clicks, before + 1);
    pins.push({ label_id: 17, point_id: id });
    assert.equal(adapter(write).state, 'ack');
}
assert.equal(clicks, 102);
assert.equal(adapter({ map: 2, write: false }).ids.length, 100);
console.log('Official UI adapter: desktop, mobile batch of 100 pins, acknowledgement, undo and domain guards passed.');

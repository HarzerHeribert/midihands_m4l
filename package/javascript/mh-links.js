// Link targets for the editor's MOVE page (a [v8] in the editor subpatcher).
//
// Follows the parameter selected in Live, so "link <k>" can link to it in one
// click, and turns target ids into names like "Drift › Filter Freq".
//
// in:  init               device is ready, start following Live's selection
//      report             the page opened: resend every name and the selection
//      label <k> id <n>   link k now points at id n (0 = nothing)
//      link <k>           link k to the selected parameter, or listen for a click
// out: mapname <k> <text>   "<none>" when unlinked
//      selected <text>      the selected parameter, "<none>" if there is none
//      setid <k> id <n>     store id n as link k's target
//      listen <k>           nothing selected: let link k wait for a click in Live

autowatch = 0;
inlets = 1;
outlets = 1;

const labels = {};
let selectedId = 0;
let selectedLabel = "<none>";
let observer = null;
let ownDevice = 0;

function idOf(value) {
  // LiveAPI hands ids back as ["id", n] or "id n".
  const parts = Array.isArray(value) ? value : String(value).split(" ");
  return Number(parts[parts.length - 1]) || 0;
}

function describe(id) {
  if (!id) return "<none>";
  const param = new LiveAPI("id " + id);
  if (!param || Number(param.id) === 0) return "<none>";
  const name = [].concat(param.get("name")).join(" ");
  let owner = new LiveAPI("id " + idOf(param.get("canonical_parent")));
  // Mixer parameters (volume, pan, sends) belong to the track's mixer.
  if (owner && owner.type === "MixerDevice") owner = new LiveAPI("id " + idOf(owner.get("canonical_parent")));
  const ownerName = owner && Number(owner.id) !== 0 ? [].concat(owner.get("name")).join(" ") : "";
  return ownerName ? ownerName + " › " + name : name;
}

function ownParameter(id) {
  const param = new LiveAPI("id " + id);
  return idOf(param.get("canonical_parent")) === ownDevice;
}

// The callback's arguments arrive as an array or as separate atoms.
function atoms(list) {
  return [].concat(...Array.from(list).map((x) => (Array.isArray(x) ? x : String(x).split(" "))));
}

function onSelected() {
  const args = atoms(arguments);
  if (args[0] !== "selected_parameter") return;  // the first call reports the observed object's own id
  const id = idOf(args);
  if (id && ownParameter(id)) return;  // clicks on this device's own controls keep the last choice
  selectedId = id;
  selectedLabel = describe(id);
  outlet(0, "selected", selectedLabel);
}

function init() {
  if (observer) return;
  ownDevice = Number(new LiveAPI("this_device").id);
  observer = new LiveAPI(onSelected, "live_set view");
  observer.property = "selected_parameter";
}

function report() {
  init();
  for (const k in labels) outlet(0, "mapname", Number(k), labels[k]);
  outlet(0, "selected", selectedLabel);
}

function label(k, word, id) {
  init();
  labels[k] = describe(Number(id) || 0);
  outlet(0, "mapname", k, labels[k]);
}

function link(k) {
  init();
  if (selectedId) outlet(0, "setid", k, "id", selectedId);
  else outlet(0, "listen", k);
}

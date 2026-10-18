// Editing state is independent of the renderer and file transport.
// Add a field normalizer to the registry before exposing a future schema field.
export class FieldRegistry {
  constructor() { this.normalizers = new Map(); }
  register(key, normalize) { this.normalizers.set(key, normalize); return this; }
  has(key) { return this.normalizers.has(key); }
  normalize(field, value) { return this.normalizers.get(field.key)(value, field); }
}
function integer(value, field, fallbackMin, fallbackMax) {
  const number = Number(value);
  const min = Number.isFinite(field.min) ? field.min : fallbackMin;
  const max = Number.isFinite(field.max) ? field.max : fallbackMax;
  if (!['number', 'string'].includes(typeof value) || (typeof value === 'string' && !value.trim())
    || !Number.isInteger(number) || number < min || number > max) {
    throw new Error(`${field.label || field.key}必须是 ${min} 到 ${max} 之间的整数。`);
  }
  return number;
}
export function createFieldRegistry() {
  return new FieldRegistry()
    .register('radius', (value, field) => integer(value, field, 0, 255))
    .register('flags', (value, field) => integer(value, field, 0, 0xffffffff) >>> 0);
}
const copyRows = (rows) => rows.map((row) => ({ ...row }));
export class WaypointEditor {
  constructor(points, schema, onChange = () => {}, registry = createFieldRegistry()) {
    this.schema = schema;
    this.registry = registry;
    this.fields = (schema.fields || []).filter((field) => registry.has(field.key));
    this.flags = (schema.flags || []).map((flag) => ({ ...flag, bit: Number(flag.bit) >>> 0 }));
    this.knownFlagsMask = Number(schema.known_flags_mask) >>> 0;
    this.editableFlagsMask = this.flags.reduce((mask, flag) => (mask | flag.bit) >>> 0, 0);
    this.sourceHash = schema.source_hash;
    this.original = points.map((point) => Object.fromEntries(this.fields.map((field) => [field.key, registry.normalize(field, point[field.key])])));
    this.values = copyRows(this.original);
    this.saved = copyRows(this.original);
    this.commands = [];
    this.cursor = 0;
    this.revision = 0;
    this.onChange = onChange;
  }
  get canUndo() { return this.cursor > 0; }
  get canRedo() { return this.cursor < this.commands.length; }
  get dirty() { return this.values.some((row, index) => this.fields.some((field) => row[field.key] !== this.saved[index][field.key])); }
  getPoint(index) {
    if (!Number.isInteger(index) || index < 0 || index >= this.values.length) throw new Error('无效的路点编号。');
    return { ...this.values[index] };
  }
  unknownFlags(index) { return (this.getPoint(index).flags & ~this.knownFlagsMask) >>> 0; }
  mergeFlags(index, checkedMask) {
    return ((this.getPoint(index).flags & ~this.editableFlagsMask) | (checkedMask & this.editableFlagsMask)) >>> 0;
  }
  apply(index, updates) {
    const current = this.getPoint(index), before = {}, after = {};
    for (const field of this.fields) {
      if (!Object.hasOwn(updates, field.key)) continue;
      const value = this.registry.normalize(field, updates[field.key]);
      if (field.key === 'flags' && ((value & ~this.knownFlagsMask) >>> 0)
        !== ((this.original[index].flags & ~this.knownFlagsMask) >>> 0)) throw new Error('未知属性位必须保留，不能修改。');
      if (current[field.key] !== value) { before[field.key] = current[field.key]; after[field.key] = value; }
    }
    if (!Object.keys(after).length) return false;
    this.commands.splice(this.cursor);
    this.commands.push({ index, before, after });
    this.cursor++;
    Object.assign(this.values[index], after);
    this.changed('apply', index);
    return true;
  }
  undo() {
    if (!this.canUndo) return false;
    const command = this.commands[--this.cursor];
    Object.assign(this.values[command.index], command.before);
    this.changed('undo', command.index);
    return true;
  }
  redo() {
    if (!this.canRedo) return false;
    const command = this.commands[this.cursor++];
    Object.assign(this.values[command.index], command.after);
    this.changed('redo', command.index);
    return true;
  }
  changed(kind, index) {
    this.revision++;
    this.onChange({ kind, index, revision: this.revision });
  }
  getChanges() {
    const changes = [];
    this.values.forEach((row, index) => {
      const change = { index };
      for (const field of this.fields) if (row[field.key] !== this.original[index][field.key]) change[field.key] = row[field.key];
      if (Object.keys(change).length > 1) changes.push(change);
    });
    return changes;
  }
  markSaved(revision) {
    if (revision !== this.revision) return false;
    this.saved = copyRows(this.values);
    this.onChange({ kind: 'saved', index: null, revision: this.revision });
    return true;
  }
}

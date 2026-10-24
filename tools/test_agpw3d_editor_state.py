"""Exercise actual browser editing state in Node without creating any files."""
import base64
import json
from pathlib import Path
import shutil
import subprocess
import unittest

from agpw3d_document import schema


@unittest.skipUnless(shutil.which('node'), 'Node.js is required for browser state checks')
class EditorStateChecks(unittest.TestCase):
    def test_vector_edit_normalization_validation_undo_and_saved_state(self):
        module = Path(__file__).with_name('agpw3d_web') / 'waypoint_editor.js'
        uri = 'data:text/javascript;base64,' + base64.b64encode(module.read_bytes()).decode('ascii')
        script = 'import { WaypointEditor } from ' + json.dumps(uri) + ';\n'
        script += 'const schema = ' + json.dumps(schema(), ensure_ascii=True) + ';\n'
        script += r'''
import assert from 'node:assert/strict';
const zeros = () => Array.from({ length: 8 }, () => [0, 0, 0]);
const point = () => ({ radius: 17, flags: 0, jumpVelocity: zeros(),
  linkSlots: Array.from({ length: 8 }, () => [-1, 0]) });
const points = [point(), point()];
points[0].linkSlots[3] = [1, 1];
points[0].linkSlots[4] = [1, 0]; // Ordinary edges remain read-only.
points[0].jumpVelocity[6][0] = -0;
const editor = new WaypointEditor(points, schema);
assert.equal(editor.dirty, false);
assert.deepEqual(editor.getChanges(), []);
const velocity = zeros();
velocity[3] = ['123.456789', '-234.56789', '300'];
assert.equal(editor.apply(0, { jumpVelocity: velocity }), true);
assert.deepEqual(editor.getPoint(0).jumpVelocity[3], [Math.fround(123.456789), Math.fround(-234.56789), 300]);
assert.equal(editor.dirty, true);
assert.equal(editor.getChanges()[0].jumpVelocity.length, 8);
assert.equal(editor.apply(0, { jumpVelocity: editor.getPoint(0).jumpVelocity }), false);
const detached = editor.getPoint(0);
detached.jumpVelocity[3][0] = 999;
assert.notEqual(editor.getPoint(0).jumpVelocity[3][0], 999);
const exported = editor.getChanges();
exported[0].jumpVelocity[3][0] = 999;
assert.notEqual(editor.getPoint(0).jumpVelocity[3][0], 999);
assert.equal(editor.undo(), true);
assert.equal(editor.dirty, false);
assert.deepEqual(editor.getChanges(), []);
assert.equal(editor.redo(), true);
assert.equal(editor.dirty, true);
assert.equal(editor.markSaved(editor.revision - 1), false);
assert.equal(editor.markSaved(editor.revision), true);
assert.equal(editor.dirty, false);
assert.equal(editor.undo(), true);
assert.equal(editor.dirty, true); // Undo after save is still an unsaved edit.
assert.deepEqual(editor.getChanges(), []); // Export is compared to source bytes.
assert.equal(editor.redo(), true);
assert.equal(editor.dirty, false);
for (const invalid of [NaN, Infinity, -Infinity, -2000.01, 2000.01, '', ' ', true, null, []]) {
  const update = editor.getPoint(0).jumpVelocity;
  update[3][0] = invalid;
  assert.throws(() => editor.apply(0, { radius: 8, jumpVelocity: update }));
  assert.equal(editor.getPoint(0).radius, 17); // Atomic validation.
  assert.equal(editor.dirty, false);
}
for (const slot of [0, 4, 6]) {
  const update = editor.getPoint(0).jumpVelocity;
  update[slot][0] = 20;
  assert.throws(() => editor.apply(0, { jumpVelocity: update }));
}
assert.throws(() => editor.apply(0, { jumpVelocity: new Array(8) }));
const missingComponent = editor.getPoint(0).jumpVelocity;
missingComponent[3] = new Array(3);
assert.throws(() => editor.apply(0, { jumpVelocity: missingComponent }));
const boundary = editor.getPoint(0).jumpVelocity;
boundary[3] = [-2000, 2000, 0];
assert.equal(editor.apply(0, { jumpVelocity: boundary }), true);
assert.deepEqual(editor.getPoint(0).jumpVelocity[3], [-2000, 2000, 0]);
assert.deepEqual(points[0].jumpVelocity[3], [0, 0, 0]); // Source snapshot remains untouched.
console.log('vector editor state passed');
'''
        result = subprocess.run(['node', '--input-type=module', '-'], input=script,
                                capture_output=True, text=True, encoding='utf-8', timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('vector editor state passed', result.stdout)


if __name__ == '__main__':
    unittest.main()

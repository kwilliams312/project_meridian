import { expect, test } from 'vitest';
import { selectBackend } from '../src/backend/index.js';

test('selectBackend returns a RenderBackend', () => {
  expect(typeof selectBackend().render).toBe('function');
});

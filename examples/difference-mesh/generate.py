"""Regenerate the small OBJ inputs used by the difference mesh examples."""
from math import sin, pi
from pathlib import Path

root = Path(__file__).resolve().parent


def write(name, points, faces):
    text = '# Woby difference mesh example; coordinates are in model units.\n'
    text += 'o ' + name + '\n'
    text += ''.join('v %.12g %.12g %.12g\n' % p for p in points)
    text += ''.join('f %d %d %d\n' % tuple(i + 1 for i in f) for f in faces)
    (root / (name + '.obj')).write_text(text, encoding='utf-8')


box = [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0),
       (0, 0, 10), (10, 0, 10), (10, 10, 10), (0, 10, 10)]
faces = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7),
         (0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
         (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
write('solid-original', box, faces)
write('solid-shifted', [(x + 2, y, z) for x, y, z in box], faces)
write('solid-distorted', [(x, y, z if z == 0 else z - 2 + .2 * (x + y))
                          for x, y, z in box], faces)


def grid(name, count, height):
    points = [(10 * x / count, 10 * y / count, height(10 * x / count, 10 * y / count))
              for y in range(count + 1) for x in range(count + 1)]
    triangles = []
    for y in range(count):
        for x in range(count):
            a = y * (count + 1) + x
            b, c, d = a + 1, a + count + 2, a + count + 1
            triangles += [(a, b, c), (a, c, d)]
    write(name, points, triangles)


grid('surface-original', 6, lambda x, y: 0)
grid('surface-raised', 10, lambda x, y: .6 + 1.4 * sin(pi * x / 10) * sin(pi * y / 10))
grid('surface-crossing', 6, lambda x, y: (x - 5) / 5)
write('surface-hole', [(0, 0, 0), (10, 0, 0), (10, 10, 0), (0, 10, 0),
                       (4, 4, 0), (6, 4, 0), (6, 6, 0), (4, 6, 0)],
      [(0, 1, 5), (0, 5, 4), (1, 2, 6), (1, 6, 5),
       (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)])

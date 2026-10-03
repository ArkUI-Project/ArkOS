#!/usr/bin/env python3
"""Offline fixed TGSI programs for ArkOS's native VirGL compositor.

Lens/gradient/dispersion/highlight equations: Kyant, Apache-2.0, pinned in
third_party/android-liquid-glass/PORT.md. ArkOS modifications: TGSI lowering,
XRGB textures, finite sigma=2 Gaussian and cubic continuous corner coverage.
"""
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parents[1]

class Shader:
    def __init__(self):
        self.code = []
        self.immediates = []
    def imm(self, *values):
        values = tuple(values if len(values) == 4 else values * 4)
        if values not in self.immediates:
            self.immediates.append(values)
        return f'IMM[{self.immediates.index(values)}]'
    def op(self, opcode, *operands):
        # TGSI source swizzles contain four lanes; destination write masks
        # may contain one to four. Preserve destination masks verbatim.
        operands = list(operands)
        for i in range(0 if opcode == 'IF' else 1, len(operands)):
            operands[i] = re.sub(r'\b((?:TEMP|CONST|IN|IMM|OUT)\[\d+\])\.([xyzw]{1,3})(?![xyzw])',
                                 lambda m: m[1]+'.'+(m[2]*4)[:4], operands[i])
        self.code.append(f'{len(self.code)}: {opcode} ' + ', '.join(operands))
    def text(self):
        head = ['FRAG', 'DCL IN[0], POSITION', 'DCL OUT[0], COLOR',
                'DCL SAMP[0]', 'DCL SAMP[1]',
                'DCL SVIEW[0], 2D, FLOAT', 'DCL SVIEW[1], 2D, FLOAT',
                'DCL CONST[0..7]', 'DCL TEMP[0..31]']
        head += [f'IMM[{i}] FLT32 {{' + ', '.join(f'{x:.9g}' for x in v) + '}'
                 for i, v in enumerate(self.immediates)]
        return '\n'.join(head + self.code + ['END']) + '\n'
    def pixel(self):
        # Exact half-integer fragment coordinates keep equal-distance corner
        # normals stable. Interpolated UV * extent introduced small tie errors.
        self.op('MOV', 'TEMP[0].xy', 'IN[0].xyxy')
        self.op('RCP', 'TEMP[1].x', 'CONST[0].z')
        self.op('RCP', 'TEMP[1].y', 'CONST[0].w')
    def sample(self, output, coordinates, sampler=0):
        # Actual extent is distinct from the retained texture allocation.
        self.op('MAX', 'TEMP[3].xy', coordinates, self.imm(.5))
        self.op('ADD', 'TEMP[4].xy', 'CONST[0].xyxy', self.imm(-.5))
        self.op('MIN', 'TEMP[3].xy', 'TEMP[3].xyxy', 'TEMP[4].xyxy')
        self.op('MUL', 'TEMP[3].xy', 'TEMP[3].xyxy', 'TEMP[1].xyxy')
        self.op('TEX', output, 'TEMP[3]', f'SAMP[{sampler}]', '2D')
    def byte_floor(self, output):
        self.op('MUL', output, output, self.imm(255))
        self.op('FLR', output, output)
        self.op('MUL', output, output, self.imm(1/255))

def preparation():
    # Shadow and vibrancy are per source pixel, before either Gaussian pass.
    # Retaining the result on the GPU avoids repeating this work at every tap.
    s = Shader(); s.pixel();s.sample('TEMP[6]', 'TEMP[0].xyxy')
    s.op('IF', 'CONST[6].x')
    # Shadow sample coordinates are clamped just like source staging.
    s.op('ADD', 'TEMP[7].xy', 'TEMP[0].xyxy', '-CONST[1].wwww')
    s.op('ADD', 'TEMP[7].xy', 'TEMP[7].xyxy', 'CONST[4].xyxy')
    s.op('MAX', 'TEMP[7].xy', 'TEMP[7].xyxy', s.imm(.5))
    s.op('ADD', 'TEMP[8].xy', 'CONST[4].zwzw', s.imm(-.5))
    s.op('MIN', 'TEMP[7].xy', 'TEMP[7].xyxy', 'TEMP[8].xyxy')
    s.op('SGE', 'TEMP[8].xy', 'TEMP[7].xyxy', 'CONST[5].xyxy')
    s.op('SLT', 'TEMP[8].zw', 'TEMP[7].xyxy', 'CONST[5].zwzw')
    s.op('MUL', 'TEMP[8].xy', 'TEMP[8].xyxy', 'TEMP[8].zwzw')
    s.op('MUL', 'TEMP[8].x', 'TEMP[8].x', 'TEMP[8].y')
    s.op('IF', 'TEMP[8].x')
    for _ in range(8):
        s.op('MAD', 'TEMP[6].xyz', 'TEMP[6]', s.imm(248/255), s.imm(7*7/65025,28*7/65025,60*7/65025,0))
        s.byte_floor('TEMP[6].xyz')
    s.op('ENDIF'); s.op('ENDIF')
    s.op('DP3', 'TEMP[7].x', 'TEMP[6]', s.imm(.213,.715,.072,0))
    s.op('MUL', 'TEMP[7].x', 'TEMP[7].x', s.imm(.5))
    s.op('MAD_SAT', 'TEMP[6].xyz', 'TEMP[6]', s.imm(1.5), '-TEMP[7].xxxx')
    s.op('MAD', 'TEMP[6].xyz', 'TEMP[6]', s.imm(255), s.imm(.5))
    s.op('FLR', 'TEMP[6].xyz', 'TEMP[6]')
    s.op('MUL', 'OUT[0].xyz', 'TEMP[6]', s.imm(1/255))
    s.op('MOV','OUT[0].w',s.imm(0))
    return s.text()

def gaussian(horizontal):
    s = Shader(); s.pixel()
    s.op('MOV', 'TEMP[5]', s.imm(0))
    for offset, weight in zip(range(-6, 7), [1,2,7,17,31,45,50,45,31,17,7,2,1]):
        s.op('ADD', 'TEMP[2].xy', 'TEMP[0].xyxy', s.imm(offset if horizontal else 0, 0 if horizontal else offset, 0, 0))
        s.sample('TEMP[6]', 'TEMP[2].xyxy')
        s.op('MAD', 'TEMP[5].xyz', 'TEMP[6]', s.imm(weight/256), 'TEMP[5]')
    s.byte_floor('TEMP[5].xyz')
    if not horizontal:
        s.op('MUL', 'TEMP[6]', 'CONST[2]', s.imm(1/255))
        s.op('LRP', 'TEMP[5].xyz', 'TEMP[6].wwww', 'TEMP[6]', 'TEMP[5]')
        s.byte_floor('TEMP[5].xyz')
    s.op('MOV', 'TEMP[5].w', s.imm(1)); s.op('MOV', 'OUT[0]', 'TEMP[5]')
    return s.text()

def coverage(s, output, inner=False):
    # Pixel centers TEMP[9] are measured from the glass center. Cubic corners
    # retain the existing continuous curvature; AA uses the same 8x8 grid.
    s.op('MOV', output, s.imm(1))
    s.op('ABS', 'TEMP[20].xy', 'TEMP[9].xyxy')
    s.op('ADD', 'TEMP[21].xy', 'TEMP[10].xyxy', '-CONST[1].zzzz')
    s.op('ADD', 'TEMP[20].xy', 'TEMP[20].xyxy', '-TEMP[21].xyxy')
    s.op('ADD', 'TEMP[20].xy', 'TEMP[20].xyxy', s.imm(-.5))
    # inner half-size and radius both shrink by one, leaving corner center
    # unchanged. Radius zero is handled as a rectangular clipping test.
    s.op('ADD', 'TEMP[21].z', 'CONST[1].z', s.imm(-1 if inner else 0))
    s.op('MAX', 'TEMP[21].z', 'TEMP[21].z', s.imm(0))
    s.op('SGE', 'TEMP[22].xy', 'TEMP[20].xyxy', s.imm(0))
    s.op('MUL', 'TEMP[22].x', 'TEMP[22].x', 'TEMP[22].y')
    s.op('IF', 'TEMP[22].x')
    s.op('MOV', output, s.imm(0))
    s.op('MUL', 'TEMP[21].w', 'TEMP[21].z', 'TEMP[21].z')
    s.op('MUL', 'TEMP[21].w', 'TEMP[21].w', 'TEMP[21].z')
    for y in range(8):
        for x in range(8):
            s.op('ADD', 'TEMP[22].xy', 'TEMP[20].xyxy', s.imm((x+.5)/8,(y+.5)/8,0,0))
            s.op('MUL', 'TEMP[23].xy', 'TEMP[22].xyxy', 'TEMP[22].xyxy')
            s.op('MUL', 'TEMP[23].xy', 'TEMP[23].xyxy', 'TEMP[22].xyxy')
            s.op('ADD', 'TEMP[23].x', 'TEMP[23].x', 'TEMP[23].y')
            s.op('SGE', 'TEMP[23].x', 'TEMP[21].w', 'TEMP[23].x')
            s.op('ADD', output, output, 'TEMP[23].x')
    s.op('MAD', output, output, s.imm(255/64), s.imm(.5))
    s.op('FLR', output, output); s.op('MUL', output, output, s.imm(1/255))
    s.op('ENDIF')
    # Straight inner edges, and outside-shape pixels.
    s.op('ABS', 'TEMP[22].xy', 'TEMP[9].xyxy')
    s.op('ADD', 'TEMP[23].xy', 'TEMP[10].xyxy', s.imm(-1 if inner else 0))
    s.op('SLT', 'TEMP[22].xy', 'TEMP[22].xyxy', 'TEMP[23].xyxy')
    s.op('MUL', output, output, 'TEMP[22].x'); s.op('MUL', output, output, 'TEMP[22].y')

def lens():
    s=Shader();s.pixel()
    s.op('MUL', 'TEMP[10].xy', 'CONST[1].xyxy', s.imm(.5))
    s.op('ADD', 'TEMP[9].xy', 'TEMP[0].xyxy', '-CONST[1].wwww')
    s.op('ADD', 'TEMP[9].xy', 'TEMP[9].xyxy', '-TEMP[10].xyxy')
    # Signed distance from the upstream circular rounded-rectangle lens.
    s.op('ABS', 'TEMP[11].xy', 'TEMP[9].xyxy')
    s.op('ADD', 'TEMP[12].xy', 'TEMP[10].xyxy', '-CONST[1].zzzz')
    s.op('ADD', 'TEMP[11].xy', 'TEMP[11].xyxy', '-TEMP[12].xyxy')
    s.op('MAX', 'TEMP[12].xy', 'TEMP[11].xyxy', s.imm(0))
    s.op('DP2', 'TEMP[13].x', 'TEMP[12].xyxy', 'TEMP[12].xyxy')
    s.op('SQRT', 'TEMP[13].x', 'TEMP[13].x')
    s.op('MAX', 'TEMP[13].y', 'TEMP[11].x', 'TEMP[11].y')
    s.op('MIN', 'TEMP[13].y', 'TEMP[13].y', s.imm(0))
    s.op('ADD', 'TEMP[13].x', 'TEMP[13].x', 'TEMP[13].y')
    s.op('ADD', 'TEMP[13].x', 'TEMP[13].x', '-CONST[1].z')
    s.op('MIN', 'TEMP[13].x', 'TEMP[13].x', s.imm(0))
    s.op('MAD_SAT', 'TEMP[13].x', 'TEMP[13].x', s.imm(1/12), s.imm(1))
    s.op('MAD', 'TEMP[13].x', '-TEMP[13].x', 'TEMP[13].x', s.imm(1))
    s.op('SQRT', 'TEMP[13].x', 'TEMP[13].x')
    s.op('ADD', 'TEMP[13].x', s.imm(1), '-TEMP[13].x')
    s.op('MUL', 'TEMP[13].x', 'TEMP[13].x', s.imm(24))
    # Gradient uses 1.5 * corner radius, exactly as upstream.
    s.op('MUL', 'TEMP[11].z', 'CONST[1].z', s.imm(1.5))
    s.op('MIN', 'TEMP[11].z', 'TEMP[11].z', 'TEMP[10].x')
    s.op('MIN', 'TEMP[11].z', 'TEMP[11].z', 'TEMP[10].y')
    s.op('ADD', 'TEMP[12].xy', 'TEMP[10].xyxy', '-TEMP[11].zzzz')
    s.op('ABS', 'TEMP[11].xy', 'TEMP[9].xyxy')
    s.op('ADD', 'TEMP[11].xy', 'TEMP[11].xyxy', '-TEMP[12].xyxy')
    s.op('MAX', 'TEMP[14].xy', 'TEMP[11].xyxy', s.imm(0))
    s.op('MAX', 'TEMP[12].z', 'TEMP[11].x', 'TEMP[11].y')
    s.op('SGE', 'TEMP[12].z', 'TEMP[12].z', s.imm(0))
    s.op('IF', 'TEMP[12].z')
    s.op('DP2', 'TEMP[14].z', 'TEMP[14].xyxy', 'TEMP[14].xyxy')
    s.op('MAX', 'TEMP[14].z', 'TEMP[14].z', s.imm(1e-20))
    s.op('RSQ', 'TEMP[14].z', 'TEMP[14].z')
    s.op('MUL', 'TEMP[14].xy', 'TEMP[14].xyxy', 'TEMP[14].zzzz')
    s.op('ELSE')
    s.op('SGE', 'TEMP[14].x', 'TEMP[11].x', 'TEMP[11].y')
    s.op('ADD', 'TEMP[14].y', s.imm(1), '-TEMP[14].x')
    s.op('ENDIF')
    s.op('SSG', 'TEMP[15].xy', 'TEMP[9].xyxy')
    s.op('MUL', 'TEMP[14].xy', 'TEMP[14].xyxy', 'TEMP[15].xyxy')
    s.op('ADD', 'TEMP[15].z', 'TEMP[14].x', 'TEMP[14].y')
    s.op('MUL', 'TEMP[15].z', 'TEMP[15].z', s.imm(.7071067811865475))
    s.op('ABS', 'TEMP[15].z', 'TEMP[15].z')
    s.op('MUL', 'TEMP[14].xy', 'TEMP[14].xyxy', 'TEMP[13].xxxx')
    s.op('ADD', 'TEMP[16].xy', 'TEMP[0].xyxy', 'TEMP[14].xyxy')
    s.op('MUL', 'TEMP[17].x', 'TEMP[9].x', 'TEMP[9].y')
    s.op('MUL', 'TEMP[17].y', 'TEMP[10].x', 'TEMP[10].y')
    s.op('RCP', 'TEMP[17].y', 'TEMP[17].y')
    s.op('MUL', 'TEMP[17].x', 'TEMP[17].x', 'TEMP[17].y')
    s.op('MUL', 'TEMP[17].x', 'TEMP[17].x', 'CONST[3].y')
    s.op('MUL', 'TEMP[17].xy', 'TEMP[14].xyxy', 'TEMP[17].xxxx')
    s.op('MOV', 'TEMP[18]', s.imm(0))
    for i, weights in enumerate([(2/7,0,0,0),(2/7,1/7,0,0),(2/7,2/7,0,0),(0,2/7,0,0),(0,2/7,1/3,0),(0,0,1/3,0),(1/7,0,1/3,0)]):
        s.op('MAD', 'TEMP[2].xy', 'TEMP[17].xyxy', s.imm((3-i)/3), 'TEMP[16].xyxy')
        s.sample('TEMP[6]', 'TEMP[2].xyxy')
        s.op('MAD', 'TEMP[18].xyz', 'TEMP[6]', s.imm(*weights), 'TEMP[18]')
    s.byte_floor('TEMP[18].xyz')
    coverage(s,'TEMP[19].x');coverage(s,'TEMP[19].y',True)
    s.op('ADD', 'TEMP[19].y', 'TEMP[19].x', '-TEMP[19].y')
    s.op('MAX', 'TEMP[19].y', 'TEMP[19].y', s.imm(0))
    s.op('MUL', 'TEMP[19].y', 'TEMP[19].y', 'TEMP[15].z')
    s.op('MUL', 'TEMP[19].y', 'TEMP[19].y', s.imm(.5))
    s.op('MUL', 'TEMP[19].y', 'TEMP[19].y', 'CONST[3].x')
    s.op('FLR', 'TEMP[19].y', 'TEMP[19].y')
    s.op('MUL', 'TEMP[19].y', 'TEMP[19].y', s.imm(1/255))
    s.op('ADD_SAT', 'TEMP[18].xyz', 'TEMP[18]', 'TEMP[19].yyyy')
    s.sample('TEMP[6]', 'TEMP[0].xyxy',1)
    s.op('LRP', 'OUT[0].xyz', 'TEMP[19].xxxx', 'TEMP[18]', 'TEMP[6]')
    s.op('MOV','OUT[0].w',s.imm(0))  # Native canvas is 0x00RRGGBB.
    return s.text()

vertex = 'VERT\nDCL IN[0]\nDCL IN[1]\nDCL OUT[0], POSITION\nDCL OUT[1], GENERIC[0]\n0: MOV OUT[0], IN[0]\n1: MOV OUT[1], IN[1]\n2: END\n'
shaders={'vertex':vertex,'preparation':preparation(),'horizontal':gaussian(True),'vertical':gaussian(False),'lens':lens()}
output='/* Generated offline by scripts/build-glass-shaders.py. */\n'
for name,text in shaders.items():
    assert len(text.encode()) < 60000, (name,len(text))
    output += f'static const char glass_{name}_shader[] =\n'
    output += '\n'.join(json.dumps(line+'\n') for line in text.splitlines())+';\n'
(ROOT/'kernel/glass_shaders.inc').write_text(output)
print('TGSI shader bytes:', {name:len(text) for name,text in shaders.items()})

"""halomap.py - minimal reader for Xbox-format Halo CE cache files (.map),
driven by Invader's tag definition JSON for struct layouts.

Spec sources:
  - ChupathingyCE's source/cache/cache_files.c (cache_file_header, cache_file_tag_header,
                                                cache_file_tag_instance, BSP header)
  - Invader src/tag/hek/definition/*.json      (field layouts of every tag group)

Only reads; never writes the map.
"""
import json, glob, os, struct, collections

# Invader's definitions (the definition JSON files): where
# HALO_PSP_INVADER_DEFINITIONS says (the bake sets it), else the bundle's own
# beside lib/, else a development checkout's
_BUNDLED = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'invader', 'definitions')
DEF_DIR = os.environ.get('HALO_PSP_INVADER_DEFINITIONS') or (
    _BUNDLED if os.path.isdir(_BUNDLED) else '/home/claude/invader/src/tag/hek/definition')

PRIM = {
    'float': 4, 'int32': 4, 'uint32': 4, 'Angle': 4, 'Fraction': 4, 'ColorARGBInt': 4,
    'Pointer': 4, 'TagID': 4, 'TagFourCC': 4, 'ScenarioScriptNodeValue': 4,
    'int16': 2, 'uint16': 2, 'Index': 2,
    'int8': 1, 'uint8': 1,
    'Point3D': 12, 'Vector3D': 12, 'Point2D': 8, 'Vector2D': 8, 'Point2DInt': 4,
    'Euler2D': 8, 'Euler3D': 12, 'Quaternion': 16, 'Plane3D': 16, 'Plane2D': 12,
    'Rectangle2D': 8, 'ColorRGB': 12, 'ColorARGB': 16, 'Matrix': 36,
    'TagString': 32, 'TagDependency': 16, 'TagReflexive': 12, 'TagDataOffset': 20,
}


class Defs:
    def __init__(self, d=DEF_DIR):
        self.structs, self.enums, self.bitfields = {}, {}, {}
        self.groups = {}   # group name -> struct name
        for fn in glob.glob(os.path.join(d, '*.json')):
            for s in json.load(open(fn, encoding='utf-8')):
                t = s['type']
                if t == 'struct':
                    self.structs[s['name']] = s
                    if s.get('class'):
                        self.groups.setdefault(s['class'], s['name'])
                        if s.get('groups'):
                            self.groups[s['class']] = s['name']
                elif t == 'enum':
                    self.enums[s['name']] = s
                elif t == 'bitfield':
                    self.bitfields[s['name']] = s
        self._fields = {}
        for n in self.structs:
            self.fields(n)

    def type_size(self, t):
        if t in PRIM:
            return PRIM[t]
        if t in self.enums:
            return 2
        if t in self.bitfields:
            return self.bitfields[t]['width'] // 8
        if t in self.structs:
            return self.structs[t]['size']
        raise KeyError(t)

    def fields(self, name):
        """flattened [(offset, field)] including inherited fields"""
        if name in self._fields:
            return self._fields[name]
        s = self.structs[name]
        out, off = [], 0
        if s.get('inherits'):
            out = list(self.fields(s['inherits']))
            off = self.structs[s['inherits']]['size']
        for f in s['fields']:
            if f['type'] == 'pad':
                off += f['size']
                continue
            sz = self.type_size(f['type'])
            if f.get('bounds'):
                sz *= 2
            sz *= f.get('count', 1)
            out.append((off, f, sz))
            off += sz
        if off != s['size']:
            raise ValueError('size mismatch %s %d != %d' % (name, off, s['size']))
        self._fields[name] = out
        return out


def fourcc(v):
    return struct.pack('>I', v).decode('latin1') if v not in (0, 0xFFFFFFFF) else '----'


# Every tag group's four-character tag -> its name. The names are Halo's own
# (the tag files' extensions in the editing kit), the spelling Invader's
# definitions (Defs.groups) and its tag index (tagindex.py) use. The tags were
# read from ChupathingyCE's source, which names each group where the engine
# uses it: a *_TAG enum in the group's definitions header (cited per line
# below by subsystem), object_types.c's object types, hs.c's script types,
# widgets.c's widget types, and the PC port's ce_models.c for the shader
# family and gbxmodel. Four groups the engine never loads (the editing kit's
# own: spheroid, input_device_defaults, preferences_network_game,
# tag_collection) are listed so that no tag in a map goes unnamed.
# tools/tag_groups_check.py checks the table against an engine checkout.
FOURCC_GROUP = {
    # objects (objects/: object_types.c, the lights, damage; items/, units/, devices/)
    'obje': 'object', 'unit': 'unit', 'bipd': 'biped', 'vehi': 'vehicle',
    'item': 'item', 'weap': 'weapon', 'eqip': 'equipment', 'garb': 'garbage',
    'proj': 'projectile', 'scen': 'scenery', 'plac': 'placeholder',
    'ssce': 'sound_scenery', 'devi': 'device', 'mach': 'device_machine',
    'ctrl': 'device_control', 'lifi': 'device_light_fixture',
    'itmc': 'item_collection', 'ligh': 'light', 'lens': 'lens_flare',
    'jpt!': 'damage_effect', 'cdmg': 'continuous_damage_effect',
    # units and AI (units/dialogue_definitions.h, ai/actor_definitions.h)
    'actr': 'actor', 'actv': 'actor_variant', 'udlg': 'dialogue',
    # widgets (objects/widgets/)
    'ant!': 'antenna', 'flag': 'flag', 'glw!': 'glow', 'elec': 'lightning',
    'mgs2': 'light_volume',
    # models, animation, collision, physics (models/, physics/)
    'mode': 'model', 'mod2': 'gbxmodel', 'antr': 'model_animations',
    'coll': 'model_collision_geometry', 'phys': 'physics', 'pphy': 'point_physics',
    # effects (effects/)
    'effe': 'effect', 'deca': 'decal', 'cont': 'contrail', 'part': 'particle',
    'pctl': 'particle_system', 'rain': 'weather_particle_system',
    'foot': 'material_effects',
    # shaders (shaders/, the PC port's ce_models.c)
    'shdr': 'shader', 'senv': 'shader_environment', 'soso': 'shader_model',
    'sotr': 'shader_transparent_generic', 'schi': 'shader_transparent_chicago',
    'scex': 'shader_transparent_chicago_extended', 'swat': 'shader_transparent_water',
    'sgla': 'shader_transparent_glass', 'smet': 'shader_transparent_meter',
    'spla': 'shader_transparent_plasma',
    # scenario and level (scenario/, structures/, game/game_globals.h,
    # camera/following_camera.c, the rasterizer's detail objects)
    'scnr': 'scenario', 'sbsp': 'scenario_structure_bsp', 'sky ': 'sky',
    'fog ': 'fog', 'wind': 'wind', 'matg': 'globals', 'trak': 'camera_track',
    'dobc': 'detail_object_collection', 'mply': 'multiplayer_scenario_description',
    # interface (interface/, hs.c's script types)
    'DeLa': 'ui_widget_definition', 'Soul': 'ui_widget_collection',
    'hudg': 'hud_globals', 'hmt ': 'hud_message_text', 'hud#': 'hud_number',
    'unhi': 'unit_hud_interface', 'wphi': 'weapon_hud_interface',
    'grhi': 'grenade_hud_interface', 'metr': 'meter', 'vcky': 'virtual_keyboard',
    # sound (sound/)
    'snd!': 'sound', 'lsnd': 'sound_looping', 'snde': 'sound_environment',
    # text and bitmaps (text/, bitmaps/)
    'ustr': 'unicode_string_list', 'str#': 'string_list', 'font': 'font',
    'bitm': 'bitmap', 'colo': 'color_table',
    # the editing kit's own, never in the engine
    'boom': 'spheroid', 'devc': 'input_device_defaults',
    'ngpr': 'preferences_network_game', 'tagc': 'tag_collection',
}


class TagInstance:
    __slots__ = ('index', 'group', 'parents', 'tag_id', 'name', 'address')


class CacheMap:
    HEADER = '<4sii4siI8s32s32s4sI'

    def __init__(self, path):
        self.path = path
        self.data = open(path, 'rb').read()
        d = self.data
        (sig, self.version, self.file_length, _, self.tag_data_offset, self.tag_data_size,
         _, name, build, _, self.checksum) = struct.unpack_from(self.HEADER, d, 0)
        assert sig == b'daeh', sig
        self.name = name.split(b'\0')[0].decode()
        self.build = build.split(b'\0')[0].decode()
        self.map_type = struct.unpack_from('<i', d, 0x60)[0]
        t = self.tag_data_offset
        (self.instances_ptr, self.scenario_id, self.tags_checksum, self.tag_count,
         self.vb_count, self.vb_ptr, self.ib_count, self.ib_ptr, tsig) = \
            struct.unpack_from('<IiIiiIiII', d, t)
        assert tsig == 0x74616773
        self.tag_base = self.instances_ptr - 0x24   # tag header sits at the window base
        self.tag_end = self.tag_base + self.tag_data_size
        self.tags = []
        for i in range(self.tag_count):
            g, p0, p1, tid, nptr, addr, _, _ = struct.unpack_from('<IIIiIIII', d, self.off(self.instances_ptr + 32 * i))
            ti = TagInstance()
            ti.index, ti.group, ti.parents, ti.tag_id = i, fourcc(g), (fourcc(p0), fourcc(p1)), tid
            ti.name = self.cstr(nptr)
            ti.address = addr
            self.tags.append(ti)
        self.by_id = {t.tag_id: t for t in self.tags}

    # address helpers -------------------------------------------------
    def in_tags(self, addr):
        return self.tag_base <= addr < self.tag_end

    def off(self, addr):
        assert self.in_tags(addr), hex(addr)
        return self.tag_data_offset + (addr - self.tag_base)

    def cstr(self, addr):
        o = self.off(addr)
        e = self.data.index(b'\0', o)
        return self.data[o:e].decode('latin1')

    def u32(self, o):
        return struct.unpack_from('<I', self.data, o)[0]

    def read(self, addr, n):
        o = self.off(addr)
        return self.data[o:o + n]


class Walker:
    """Walks a tag's struct tree with the JSON defs.  Calls back for every
    block (reflexive) and data blob with the byte range it occupies.

    For tags living outside the tag window (the BSP), pass a translate()
    function mapping a pointer to a file offset."""

    def __init__(self, defs, data, translate):
        self.defs, self.data, self.tr = defs, data, translate

    def walk(self, struct_name, file_off, count, path, cb, depth=0):
        defs = self.defs
        size = defs.structs[struct_name]['size']
        fields = defs.fields(struct_name)
        for i in range(count):
            base = file_off + i * size
            for off, f, sz in fields:
                t = f['type']
                o = base + off
                if t == 'TagReflexive':
                    n, ptr = struct.unpack_from('<iI', self.data, o)
                    if n <= 0 or ptr == 0:
                        continue
                    fo = self.tr(ptr)
                    sub = f['struct']
                    p = path + '/' + f['name']
                    if fo is None:
                        cb('badptr', p, ptr, n * defs.structs[sub]['size'], sub, n)
                        continue
                    cb('block', p, fo, n * defs.structs[sub]['size'], sub, n)
                    self.walk(sub, fo, n, p, cb, depth + 1)
                elif t == 'TagDataOffset':
                    n, ext, foff, ptr = struct.unpack_from('<iIiI', self.data, o)
                    if n <= 0:
                        continue
                    p = path + '/' + f['name']
                    fo = self.tr(ptr) if ptr else None
                    if fo is not None:
                        cb('data', p, fo, n, None, 1)
                    else:
                        cb('extdata', p, foff, n, (ext, ptr), 1)
                elif t == 'TagDependency':
                    g, _, _, tid = struct.unpack_from('<IIii', self.data, o)
                    if tid != -1 and g:
                        cb('dep', path + '/' + f['name'], tid, 0, fourcc(g), 1)
                elif t in defs.structs and t not in PRIM:
                    self.walk(t, o, f.get('count', 1), path + '/' + (f.get('name') or t), cb, depth + 1)

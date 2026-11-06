"""Assertions for tests/readback_smoke.cpp, shared by the proxy and startup-injection suites."""
import json
import struct

from inspector import analyze, constant_rows, disassemble, read_capture, read_dds


def vs_constants():
    rows = [(float(k), k + 0.5, float(-k), 7.0) for k in range(32)]
    rows[16] = (0.25, 0.5, 0.75, 1.0)
    return rows


def validate_readback(session):
    captures = list(session.glob('capture_*.jsonl'))
    assert len(captures) == 1, captures
    capture = captures[0]
    header = json.loads(capture.read_text(encoding='utf8').splitlines()[0])
    assert header['version'] == 5, header
    assert header['options'] == {'constants': 1, 'textures': 1, 'buffers': 0, 'buffer_limit': 65536, 'frames': 0, 'only_matching': 0, 'filter': ['Readback'],
                                 'data': 'capture_1_data/'}, header
    events, complete, errors = read_capture(capture)
    assert complete and not errors and len(events) == 2, (complete, errors, len(events))
    matched, unmatched = events
    assert matched['shader_name'] == 'Readback/Match' and matched.get('readback') == 1, matched
    assert unmatched['shader_name'] == 'Other/NoMatch' and 'readback' not in unmatched, unmatched
    for event in events:
        assert event['vs_shader_name'] == 'Shared/VS', event
        assert event['vs_shader_file'].startswith('vs_') and (session / event['vs_shader_file']).is_file(), event

    def blob(entry):
        return (session / entry['data']).read_bytes()

    # VS constants: D3D11.1 offset binding of constants 16..31 is reported and read exactly.
    vs_cb = matched['vs_constant_buffers']
    assert len(vs_cb) == 1 and vs_cb[0]['first_constant'] == 16 and vs_cb[0]['num_constants'] == 16, vs_cb
    assert vs_cb[0]['data_offset'] == 256 and vs_cb[0]['data_bytes'] == 256, vs_cb
    expected = b''.join(struct.pack('<4f', *row) for row in vs_constants()[16:])
    assert blob(vs_cb[0]) == expected
    rows = constant_rows(session, capture, matched['event'], 'vs', 0)
    assert rows[0] == (16, (0.25, 0.5, 0.75, 1.0)) and rows[1] == (17, (17.0, 17.5, -17.0, 7.0)), rows[:2]
    ps_cb = matched['constant_buffers']
    assert len(ps_cb) == 1 and blob(ps_cb[0]) == struct.pack('<4f', 0.125, 0.375, 0.625, 0.875), ps_cb

    # Textures: mip 0 as DDS with the bound view format; payload equals the uploaded bytes.
    rgba = bytes((j * 7 + 3) & 255 for j in range(64))
    bc1 = bytes((j * 13 + 5) & 255 for j in range(32))
    heights = struct.pack('<4f', 1.5, -2.25, 3.0, 1000.0)
    for entry, (fmt, size, payload) in zip(matched['inputs'], [(28, 4, rgba), (71, 8, bc1)]):
        info = read_dds(session / entry['dump'])
        assert (info['format'], info['width'], info['height'], info['data']) == (fmt, size, size, payload), info
    info = read_dds(session / matched['vs_inputs'][0]['dump'])
    assert (info['format'], info['width'], info['data']) == (41, 2, heights), info

    # Vertex streams: stride, per-vertex positions and per-instance data.
    streams = matched['vertex_buffers']
    assert [(s['slot'], s['stride'], s['offset']) for s in streams] == [(0, 8, 0), (1, 16, 0)], streams
    assert blob(streams[0]) == struct.pack('<6f', -1, -1, -1, 3, 3, -1)
    assert blob(streams[1]) == struct.pack('<8f', *range(1, 9))
    assert matched['index_buffer'] is None

    # The unmatched draw keeps bindings but performs no readback.
    for key in ['constant_buffers', 'vs_constant_buffers', 'vertex_buffers']:
        assert all('data' not in item for item in unmatched[key]), (key, unmatched[key])
    for key in ['inputs', 'vs_inputs']:
        assert all('dump' not in item and 'dump_skipped' not in item for item in unmatched[key]), unmatched[key]
        assert [i['resource']['id'] for i in unmatched[key]] == [i['resource']['id'] for i in matched[key]]

    data = session / 'capture_1_data'
    assert len(list(data.glob('tex_*.dds'))) == 3 and len(list(data.glob('buf_*.bin'))) == 4, list(data.iterdir())
    analyze(session)
    assert (session / matched['inputs'][0]['dump']).with_suffix('.png').read_bytes()[:8] == b'\x89PNG\r\n\x1a\n'
    assert not (session / matched['inputs'][1]['dump']).with_suffix('.png').exists()  # BC1 stays DDS
    tsv = capture.with_suffix('.topology.tsv').read_text(encoding='utf8')
    assert 'vs_' in tsv and 'tex_' in tsv, tsv
    disassemble(session)
    assert len(list((session / 'disassembly').glob('vs_*.txt'))) == 1

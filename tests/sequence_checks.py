"""GPU-verified changing SRVs, view ranges, filtering and Present intervals."""
import json
import struct
from inspector import read_capture, analyze, disassemble


def validate_sequence(session, modern):
    path = session/'capture_1.jsonl'
    rows = [json.loads(s) for s in path.read_text(encoding='utf8').splitlines()]
    events, complete, errors = read_capture(path)
    assert complete and not errors and len(events) == 6, (complete, errors, len(events))
    assert rows[0]['version'] == 5 and rows[0]['qpc_frequency'] > 0
    assert rows[0]['options']['buffer_limit'] == 131072
    assert rows[-1]['reason'] == 'present_limit' and rows[-1]['presents'] == 3, rows[-1]
    markers = [r for r in rows if r.get('type') == 'present']
    assert len(markers) == 3 and all(r['flags'] == 0 for r in markers), markers
    assert {r['method'] for r in markers} == ({'Present1'} if modern else {'Present'}), markers
    assert len({r['swap_chain'] for r in markers}) == 1
    seq = markers[0]['present_seq'] - 1
    assert [r['present_seq'] for r in markers] == [seq+1, seq+2, seq+3]
    assert [e['present_seq'] for e in events] == [seq, seq, seq+1, seq+1, seq+2, seq+2]
    assert all(e['present_tracking'] == 1 for e in events)
    hashes = []
    for frame in range(3):
        draw, dispatch = events[frame*2:frame*2+2]
        assert draw['call'] == 'Draw' and dispatch['call'] == 'Dispatch'
        assert draw['shader_name'] == 'SequenceMatch/PS' and dispatch['shader_name'] == 'SequenceMatch/CS'
        for key, event, slot, offset, size, stride in [
            ('vs_inputs',draw,0,64,64,32),('vs_inputs',draw,1,16,16,4),
            ('inputs',draw,2,16,16,16),('inputs',dispatch,0,64,64,32),
            ('inputs',dispatch,1,16,16,4),('inputs',dispatch,2,16,16,16)]:
            entry = next(x for x in event[key] if x['slot']==slot)
            assert (entry['data_offset'],entry['data_bytes'],entry['element_bytes']) == (offset,size,stride), entry
            blob = (session/entry['data']).read_bytes()
            if slot == 0:
                assert blob[:16] == struct.pack('<4f',(frame+1)/16,0.125,0.25,0.5)
                assert entry['resource']['structure_stride'] == 32 and entry['resource']['name']=='Test/Bones'
            else:
                assert blob == struct.pack('<4f',*([0.0625]*4))
            if slot == 1: assert entry['raw'] == 1
        hashes.append(draw['vs_inputs'][0]['data'])
        large=next(x for x in draw['vs_inputs'] if x['slot']==3)
        assert (large['data_offset'],large['data_bytes'])==(16,79984) and not large.get('data_truncated'),large
        assert (session/large['data']).read_bytes()==struct.pack('<19996I',*(i^0x1badcafe for i in range(4,20000)))
    assert len(set(hashes)) == 3, hashes  # Same resource, changed data must not reuse the first snapshot.
    assert len({e['vs_inputs'][0]['resource']['id'] for e in events if e['call']=='Draw'}) == 1
    analyze(session)
    tsv = path.with_suffix('.topology.tsv').read_text(encoding='utf8')
    assert 'present_seq\tqpc' in tsv and 'VS t0 64 B' in tsv
    disassemble(session)

"""Assertions shared by both launch paths for the native state/argument exercise."""
import csv
import json

from inspector import analyze, disassemble, read_capture


def validate_state_args(session):
    captures = list(session.glob('capture_*.jsonl'))
    assert len(captures) == 1, captures
    capture = captures[0]
    header = json.loads(capture.read_text(encoding='utf8').splitlines()[0])
    assert header['version'] == 5, header
    events, complete, errors = read_capture(capture)
    assert complete and not errors and len(events) == 14, (complete, errors, len(events))
    direct = [
        ('DrawIndexed', {'index_count': 3, 'start_index': 1, 'base_vertex': -2}),
        ('DrawInstanced', {'vertex_count': 3, 'instance_count': 2, 'start_vertex': 5, 'start_instance': 7}),
        ('DrawIndexedInstanced', {'index_count': 3, 'instance_count': 2, 'start_index': 1,
                                  'base_vertex': -2, 'start_instance': 11}),
    ]
    for event, (call, args) in zip(events, direct):
        assert event['call'] == call and event['args'] == args, event
    for index, call, offset, size in [(3, 'DrawInstancedIndirect', 16, 64),
                                       (4, 'DrawIndexedInstancedIndirect', 32, 64),
                                       (13, 'DispatchIndirect', 12, 32)]:
        event = events[index]
        assert event['call'] == call, event
        assert event['args']['offset'] == offset, event
        buffer = event['args']['buffer']
        assert buffer['dimension'] == 1 and buffer['bytes'] == size and buffer['id'], buffer
    assert events[3]['args']['buffer']['id'] == events[4]['args']['buffer']['id']
    assert events[3]['args']['buffer']['id'] != events[13]['args']['buffer']['id']
    assert events[3]['indirect_arguments']['resolved'] == direct[1][1]
    assert events[4]['indirect_arguments']['resolved'] == direct[2][1]
    assert events[13]['indirect_arguments']['resolved'] == {'x': 4, 'y': 2, 'z': 1}
    for i, size in [(3,16),(4,20),(13,12)]:
        data=events[i]['indirect_arguments']
        assert data['data_bytes']==size and data['data_offset']==events[i]['args']['offset'],data
        assert (session/data['data']).stat().st_size==size
    no_depth = {0, 1, 2, 3, 4, 10}
    depth_id = None
    for index, event in enumerate(events[:13]):
        assert event['context_type'] == 0, event
        assert event['outputs'][0]['resource']['width'] == event['outputs'][0]['resource']['height'] == 4
        if index >= 5:
            assert event['call'] == 'Draw' and event['args'] == {'vertex_count': 3, 'start_vertex': 5}, event
        if index in no_depth:
            assert event['depth_target'] is None, event
        else:
            target = event['depth_target']
            assert target['view_format'] == 45, target
            resource = target['resource']
            assert resource['width'] == resource['height'] == 4 and resource['format'] == 44, resource
            if depth_id is None:
                depth_id = resource['id']
            assert resource['id'] == depth_id, resource
            flags = index - 6 if index in (7, 8, 9) else 3 if index == 12 else 0
            assert target['flags'] == flags, target
        if index in (7, 8, 9, 12):
            expected = dict(default=0, depth_enable=1, depth_write=0, depth_func=4,
                            stencil_enable=1, stencil_ref=0x5b if index == 12 else 0x5a)
        elif index == 11:
            expected = dict(default=0, depth_enable=0, depth_write=1, depth_func=2,
                            stencil_enable=0, stencil_ref=0)
        else:
            expected = dict(default=1, depth_enable=1, depth_write=1, depth_func=2,
                            stencil_enable=0, stencil_ref=9 if index in (5, 6) else 0)
        assert event['depth_state'] == expected, (index, event['depth_state'])
        assert event['shader_file'].startswith('ps_') and (session / event['shader_file']).is_file()
    compute = events[13]
    assert compute['context_type'] == 0
    assert 'depth_target' not in compute and 'depth_state' not in compute, compute
    assert compute['outputs'][0]['resource']['width'] == 4 and compute['outputs'][0]['resource']['height'] == 2
    assert compute['shader_file'].startswith('cs_') and (session / compute['shader_file']).is_file()
    assert len(list(session.glob('*.dxbc'))) == 3  # VS, PS, CS
    analyze(session)
    disassemble(session)
    with capture.with_suffix('.topology.tsv').open(encoding='utf8', newline='') as stream:
        table = list(csv.DictReader(stream, delimiter='\t'))
    assert len(table) == 14
    assert 'base_vertex=-2' in table[0]['args']
    assert '只读标志 3' in table[9]['depth'] and 'ref=90' in table[9]['depth']
    assert '默认状态' in table[5]['depth']
    assert 'x=4, y=2, z=1' in table[13]['args'] and table[13]['depth'] == ''
    assert 'base_vertex=-2' in table[4]['args']

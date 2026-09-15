"""Controller-side inventory validation and endpoint compilation; no host mutations.

Inventory is ordinary Ansible YAML. Instance documents are explicitly loaded
data, not inventory plugins, shell fragments, or automatically executed scripts.
"""
from __future__ import annotations

import copy
import hashlib
import ipaddress
import json
import re
from pathlib import Path

import yaml

BASE = Path('/var/lib/tuntom-deploy/instances')
RUN = Path('/run/tuntom-deploy')
KINDS = {'tunnels': 'tunnel', 'switches': 'switch', 'adapters': 'adapter'}
NAME = re.compile(r'[a-zA-Z0-9][a-zA-Z0-9_-]{0,31}\Z')


class Invalid(ValueError):
    pass


class UniqueLoader(yaml.SafeLoader):
    pass


def unique_mapping(loader, node, deep=False):
    result = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if not isinstance(key, str) or key in result:
            raise Invalid(f'duplicate or non-string YAML key: {key!r}')
        result[key] = loader.construct_object(value_node, deep=deep)
    return result


UniqueLoader.add_constructor('tag:yaml.org,2002:map', unique_mapping)


def read_yaml(path):
    try:
        result = yaml.load(Path(path).read_text(), Loader=UniqueLoader)
    except yaml.YAMLError as error:
        raise Invalid(f'invalid YAML in {path}') from error
    if not isinstance(result, dict):
        raise Invalid(f'{path}: expected a mapping')
    return result


def fields(value, allowed, label):
    if not isinstance(value, dict):
        raise Invalid(f'{label}: expected a mapping')
    extra = value.keys() - set(allowed.split())
    if extra:
        raise Invalid(f'{label}: unknown fields: {", ".join(sorted(extra))}')


def number(value, low, high, label):
    if type(value) is not int or not low <= value <= high:
        raise Invalid(f'{label}: expected integer {low}..{high}')
    return value


def boolean(value, label):
    if type(value) is not bool:
        raise Invalid(f'{label}: expected true or false')
    return value


def name(value):
    if not isinstance(value, str) or not NAME.fullmatch(value):
        raise Invalid(f'invalid name: {value!r}')
    return value


def clean_text(value, label):
    if not isinstance(value, str) or not value or any(ord(c) < 32 for c in value):
        raise Invalid(f'{label}: expected nonempty single-line text')
    return value


def socket_path(value):
    clean_text(value, 'socket')
    if not value.startswith('/') or '..' in Path(value).parts or len(value.encode()) > 107:
        raise Invalid('socket: expected absolute path of at most 107 bytes')
    return value


def inside(root, relative, must_exist=True):
    """No traversal, symlinks, special files, or shared external asset ownership."""
    root = Path(root).absolute()
    if root.is_symlink():
        raise Invalid(f'symlink instance root: {root}')
    if not isinstance(relative, str) or not relative:
        raise Invalid('empty asset path')
    part = Path(relative)
    if part.is_absolute() or '..' in part.parts or part == Path('.'):
        raise Invalid(f'asset must stay inside its instance: {relative}')
    current = root
    for item in part.parts:
        current = current / item
        if current.is_symlink():
            raise Invalid(f'symlink asset is not supported: {relative}')
    if must_exist and not current.is_file():
        raise Invalid(f'missing regular asset: {relative}')
    return current


def asset_digest(directory):
    digest = hashlib.sha256()
    for path in sorted(Path(directory).rglob('*')):
        relative = path.relative_to(directory)
        if path.is_symlink():
            raise Invalid(f'symlink in instance: {relative}')
        if path.is_dir():
            continue
        if not path.is_file():
            raise Invalid(f'non-regular file: {relative}')
        # Secrets have their own restricted transfer; never export their hashes.
        if relative.parts[0] == 'secrets':
            continue
        digest.update(str(relative).encode() + b'\0' + path.read_bytes())
    return digest.hexdigest()


def load_instances(inventory):
    result = []
    for plural, kind in KINDS.items():
        for path in sorted((Path(inventory) / 'instances' / plural).glob('*/instance.yml')):
            if any(p.is_symlink() for p in [path, *path.parents] if p == Path(inventory) or Path(inventory) in p.parents):
                raise Invalid(f'symlink instance: {path}')
            value = read_yaml(path)
            fields(value, 'tuntom_instance', str(path))
            result.append(Instance(kind, name(path.parent.name), path.parent, value['tuntom_instance']))
    keys = [item.key for item in result]
    if len(keys) != len(set(keys)):
        raise Invalid('duplicate instance identity')
    return result


class Instance:
    def __init__(self, kind, alias, directory, definition):
        self.kind, self.alias = kind, name(alias)
        self.key = f'{kind}-{alias}'
        self.directory = Path(directory)
        self.definition = copy.deepcopy(definition)
        self.root = BASE / self.key
        self.run = RUN / self.key
        common = 'schema host pre_hook post_hook group_pre_hook group_post_hook mtu socket_owner'
        if kind == 'tunnel':
            allowed = common + ' tunnel_id count transport_mtu no_address crypto_auth_only no_stats prefix16 snat mss_clamp mark mask table chain client server secret_file all_tools'
        elif kind == 'switch':
            allowed = common + ' implementation socket rules_file options auto_pool reserve_cpus'
        elif kind == 'adapter':
            allowed = common + ' interface switch_socket port_id classifier_file options'
        else:
            raise Invalid(f'unknown component: {kind}')
        fields(definition, allowed, self.key)
        if definition.get('schema') != 1:
            raise Invalid(f'{self.key}: schema must be 1')
        self.mtu = number(definition.get('mtu', 1500), 576, 65535, 'mtu')
        for field in ('pre_hook', 'post_hook', 'group_pre_hook', 'group_post_hook'):
            if definition.get(field):
                self.asset(definition[field])
        owner = definition.get('socket_owner', 'tuntom:tuntom')
        if not re.fullmatch(r'[a-zA-Z0-9_-]+:[a-zA-Z0-9_-]+', owner):
            raise Invalid('socket_owner must be user:group')
        if kind == 'tunnel':
            self.validate_tunnel()
        else:
            name(definition.get('host'))
            if kind == 'switch':
                if definition.get('implementation', 'mp') not in ('single', 'mp'):
                    raise Invalid('implementation must be single or mp')
                socket_path(definition.get('socket', f'/run/tuntom/{alias}.sock'))
                if definition.get('rules_file'):
                    self.asset(definition['rules_file'])
                boolean(definition.get('auto_pool', False), 'auto_pool')
                if definition.get('auto_pool') and definition.get('implementation', 'mp') != 'mp':
                    raise Invalid('auto_pool requires the mp implementation')
                if 'reserve_cpus' in definition:
                    number(definition['reserve_cpus'], 0, 65535, 'reserve_cpus')
                    if not definition.get('auto_pool'):
                        raise Invalid('reserve_cpus requires auto_pool')
            else:
                interface = name(definition.get('interface', alias))
                if len(interface) > 15:
                    raise Invalid('interface must fit Linux IFNAMSIZ')
                socket_path(definition.get('switch_socket'))
                self.port(definition.get('port_id'))
                if definition.get('classifier_file'):
                    self.asset(definition['classifier_file'])
            self.options()

    def asset(self, relative):
        inside(self.directory, relative)
        if Path(relative).parts[0] == 'secrets':
            raise Invalid('secret files cannot be used as public assets')
        return str(self.root / 'assets' / relative)

    @staticmethod
    def port(value):
        clean_text(value, 'port_id')
        if len(value.encode()) > 63 or any(c.isspace() for c in value):
            raise Invalid('port_id: expected 1..63 bytes without whitespace')
        return value

    def validate_tunnel(self):
        d = self.definition
        ident = number(d.get('tunnel_id'), 1, 255, 'tunnel_id')
        count = number(d.get('count', 1), 1, 64, 'count')
        number(d.get('transport_mtu', 1400), 500, 65535, 'transport_mtu')
        for flag, default in [('no_address', False), ('crypto_auth_only', False), ('no_stats', False), ('snat', False), ('mss_clamp', True), ('all_tools', False)]:
            boolean(d.get(flag, default), flag)
        prefix = d.get('prefix16', '10.254')
        try:
            ipaddress.IPv4Address(f'{prefix}.0.0')
        except (ValueError, TypeError) as error:
            raise Invalid('prefix16 must contain two IPv4 octets') from error
        if len(prefix.split('.')) != 2:
            raise Invalid('prefix16 must contain two IPv4 octets')
        if count > 1 and any(x in d for x in ('mark', 'mask', 'table')):
            raise Invalid('multiple members require automatic mark, mask and table')
        for field in ('mark', 'mask', 'table'):
            if field in d:
                number(d[field], 1, 0xffffffff, field)
        if d.get('table') in (253,254,255):
            raise Invalid('reserved Linux routing tables cannot belong to an instance')
        if not re.fullmatch(r'[A-Za-z0-9_]{1,20}', d.get('chain', f'TUNTOM_{ident}')):
            raise Invalid('chain: expected 1..20 letters, digits or underscores')
        secret = d.get('secret_file', 'secrets/master.key')
        if not isinstance(secret, str) or len(Path(secret).parts) != 2 or Path(secret).parts[0] != 'secrets':
            raise Invalid('secret_file must be a file directly inside secrets/')
        inside(self.directory, secret, must_exist=False)
        for side in ('client', 'server'):
            endpoint = d.get(side)
            fields(endpoint, 'host peer_address switch_socket port_id label exit_node ipc ipc_batch classifier_file pre_hook post_hook', side)
            name(endpoint.get('host'))
            if side == 'client':
                clean_text(endpoint.get('peer_address'), 'client.peer_address')
                if endpoint['peer_address'].startswith('-'):
                    raise Invalid('peer_address cannot start with a dash')
            for field in ('classifier_file', 'pre_hook', 'post_hook'):
                if endpoint.get(field):
                    self.asset(endpoint[field])
            if endpoint.get('switch_socket'):
                socket_path(endpoint['switch_socket'])
                self.port(endpoint.get('port_id'))
                self.port(endpoint['port_id'] + (f'_{count-1}' if count > 1 else ''))
                number(endpoint.get('label'), 0, 2**64-1, 'label')
                boolean(endpoint.get('exit_node', False), 'exit_node')
                if endpoint.get('ipc', 'auto') not in ('auto', 'v1', 'inline'):
                    raise Invalid('ipc must be auto, v1 or inline')
                number(endpoint.get('ipc_batch', 8), 1, 16, 'ipc_batch')
            elif any(k in endpoint for k in ('port_id', 'label', 'exit_node', 'ipc', 'ipc_batch', 'classifier_file')):
                raise Invalid(f'{side}: switch options require switch_socket')
        if d['client']['host']==d['server']['host'] and all(not d[s].get('switch_socket') or d[s].get('exit_node',False) for s in ('client','server')):
            raise Invalid('two TUN endpoints on one host would share routing tables; use distinct hosts or switch-only endpoints')

    def options(self):
        options = self.definition.get('options', {})
        if not isinstance(options, dict):
            raise Invalid('options must be a mapping')
        if self.kind == 'switch':
            numeric = {'max-ports': (1,65535), 'max-pending': (1,65535)}
            if self.definition.get('implementation', 'mp') == 'mp':
                numeric.update({key:(1,65535) for key in ('workers','work-per-thread','rx-weight','tx-weight','adapter-weight','trunk-weight','pool-size','queue-size')})
                numeric.update({'ipc-batch':(1,16), 'ipc-slots':(1,128), 'ipc-frame-capacity':(17,65607), 'ipc-memory-mib':(1,65535)})
            enums = {'default-back': ('on','off')}
            if self.definition.get('implementation', 'mp') == 'mp':
                enums['ipc-mode'] = ('auto','v1','inline')
        else:
            numeric = {'l4-capacity':(1,100000000), 'l3-capacity':(1,100000000), 'l4-timeout':(1,86400), 'l3-timeout':(1,86400), 'switch-ipc-batch':(1,16)}
            enums = {'switch-ipc': ('auto','v1','inline')}
        result = []
        for key, value in options.items():
            if key in numeric:
                number(value, *numeric[key], key)
            elif key in enums:
                if value not in enums[key]:
                    raise Invalid(f'invalid {key}')
            else:
                raise Invalid(f'unsupported option: {key}; use named fields for paths')
            result += [f'--{key}={value}'] if key == 'default-back' else [f'--{key}', str(value)]
        return result

    def hosts(self):
        d = self.definition
        return sorted({d[s]['host'] for s in ('client','server')}) if self.kind == 'tunnel' else [d['host']]

    def endpoint(self, endpoint_id, args, env, **extra):
        d = self.definition
        return {'id': endpoint_id, 'selector': f'{self.kind}:{endpoint_id}',
                'unit': f'tuntom-{self.key}-{endpoint_id}.service',
                'args': args, 'env': {k:str(v) for k,v in env.items()},
                'control': str(self.run / f'{endpoint_id}.control'),
                'stats': str(self.run / f'{endpoint_id}.stats'),
                'socket_owner': d.get('socket_owner','tuntom:tuntom'), **extra}

    def compile(self, host):
        d = self.definition
        endpoints = []
        if self.kind == 'tunnel':
            ident, count, prefix = d['tunnel_id'], d.get('count',1), d.get('prefix16','10.254')
            members = ' '.join(str(ident) + (f'_{i}' if i else '') for i in range(count))
            for side, letter in [('client','c'), ('server','s')]:
                endpoint = d[side]
                if endpoint['host'] != host:
                    continue
                for index in range(count):
                    member = str(ident) + (f'_{index}' if index else '')
                    key, h = ident + index*256, 4*index+1
                    addresses = [f'{prefix}.{ident}.{h}', f'{prefix}.{ident}.{h+1}']
                    addresses6 = [f'fd42::{prefix.replace(".", ":")}:{ident}:{h:x}', f'fd42::{prefix.replace(".", ":")}:{ident}:{h+1:x}']
                    if d.get('no_address',False):
                        addresses = addresses6 = ['', '']
                    local = 0 if side == 'client' else 1
                    interface = f'ut{member}{letter}'
                    has_tun = not endpoint.get('switch_socket') or endpoint.get('exit_node',False)
                    args = [side, member, interface]
                    if side == 'client':
                        args += [endpoint['peer_address']]
                    args += ['--mtu',str(self.mtu),'--transport-mtu',str(d.get('transport_mtu',1400))]
                    if d.get('crypto_auth_only'): args += ['--crypto-auth-only']
                    if d.get('no_stats'): args += ['--no-stats']
                    port = ''
                    if endpoint.get('switch_socket'):
                        port = endpoint['port_id'] + (f'_{index}' if index else '')
                        args += ['--switch-socket',endpoint['switch_socket'],'--switch-port-id',port,'--switch-label',str(endpoint['label']), '--switch-ipc',endpoint.get('ipc','auto'),'--switch-ipc-batch',str(endpoint.get('ipc_batch',8))]
                        if endpoint.get('exit_node'): args += ['--switch-exit-node']
                        if endpoint.get('classifier_file'): args += ['--classifier-file',self.asset(endpoint['classifier_file'])]
                    chain = d.get('chain', f'TUNTOM_{ident}') + (f'_{index}' if index else '')
                    env = {'TUNTOM_ID':ident,'TUNTOM_GROUP_ID':ident,'TUNTOM_INSTANCE':member,
                           'TUNTOM_INSTANCE_KEY':key,'TUNTOM_MEMBER_INDEX':index,'TUNTOM_MEMBER_COUNT':count,
                           'TUNTOM_MEMBERS':members,'TUNTOM_SCOPE':'member','TUNTOM_SIDE':'local' if local==0 else 'remote',
                           'TUNTOM_IF':interface if has_tun else '', 'TUNTOM_NO_ADDRESS':int(d.get('no_address',False)),
                           'TUNTOM_CLIENT_IP':addresses[0], 'TUNTOM_SERVER_IP':addresses[1],
                           'TUNTOM_CLIENT_IPV6':addresses6[0],'TUNTOM_SERVER_IPV6':addresses6[1],
                           'TUNTOM_LOCAL_IP':addresses[local],'TUNTOM_PEER_IP':addresses[1-local],
                           'TUNTOM_LOCAL_IPV6':addresses6[local],'TUNTOM_PEER_IPV6':addresses6[1-local],
                           'TUNTOM_UDP_PORT':40000+key,'TUNTOM_MTU':self.mtu,
                           'TUNTOM_TRANSPORT_MTU':d.get('transport_mtu',1400), 'TUNTOM_SNAT':int(d.get('snat',False)),
                           'TUNTOM_MSS_CLAMP':int(d.get('mss_clamp',True)), 'TUNTOM_MARK':d.get('mark',key<<16),
                           'TUNTOM_MARK_MASK':d.get('mask',0xffff0000),'TUNTOM_TABLE':d.get('table',10000+key),
                           'TUNTOM_CHAIN':chain, 'TUNTOM_SWITCH_SOCKET':endpoint.get('switch_socket',''),
                           'TUNTOM_SWITCH_PORT_ID':port, 'TUNTOM_FILES_DIR':str(self.root/'assets'/'files'/side)}
                    for label, suffix in [('NAT','N'),('SNAT','S'),('MANGLE','M'),('FORWARD','F')]:
                        env[f'TUNTOM_{label}_CHAIN'] = f'{chain}_{suffix}'
                    endpoints.append(self.endpoint(member+letter,args,env,role=side,interface=interface if has_tun else '',
                        pre_hook=endpoint.get('pre_hook',d.get('pre_hook','')),post_hook=endpoint.get('post_hook',d.get('post_hook',''))))
        else:
            interface = d.get('interface',self.alias) if self.kind=='adapter' else ''
            args = self.options()
            env = {'TUNTOM_COMPONENT':self.kind,'TUNTOM_ID':self.alias,'TUNTOM_SIDE':'local','TUNTOM_IF':interface,'TUNTOM_MTU':self.mtu,
                   'TUNTOM_FILES_DIR':str(self.root/'assets'/'files'), 'TUNTOM_SOCKET_OWNER':d.get('socket_owner','tuntom:tuntom')}
            if self.kind=='switch':
                args += ['--socket',d.get('socket',f'/run/tuntom/{self.alias}.sock')]
                if d.get('rules_file'): args += ['--rules-file',self.asset(d['rules_file'])]
                env.update(TUNTOM_SWITCH_SOCKET=args[args.index('--socket')+1], TUNTOM_RULES_FILE=self.asset(d['rules_file']) if d.get('rules_file') else '')
            else:
                args = [interface,'--mtu',str(self.mtu),'--switch-socket',d['switch_socket'],'--switch-port-id',d['port_id']] + args
                if d.get('classifier_file'): args += ['--classifier-file',self.asset(d['classifier_file'])]
                env.update(TUNTOM_SWITCH_SOCKET=d['switch_socket'],TUNTOM_SWITCH_PORT_ID=d['port_id'])
            endpoints.append(self.endpoint(self.alias,args,env,role=self.kind,interface=interface,pre_hook=d.get('pre_hook',''),post_hook=d.get('post_hook','')))
        return {'schema':1,'key':self.key,'kind':self.kind,'alias':self.alias,'host':host,
                'root':str(self.root),'run':str(self.run),'target':f'tuntom-{self.key}.target',
                'implementation':d.get('implementation','mp'),'endpoints':endpoints,
                'group_pre_hook':d.get('group_pre_hook',''),'group_post_hook':d.get('group_post_hook',''),
                'auto_pool':d.get('auto_pool',False),'reserve_cpus':d.get('reserve_cpus'),
                'all_tools':d.get('all_tools',False),
                'manifest':self.manifest() if self.kind=='tunnel' else '',
                'secret_file':d.get('secret_file','secrets/master.key') if self.kind=='tunnel' else '',
                'source_digest':asset_digest(self.directory)}

    def manifest(self):
        """The TSV contract consumed by existing mk_tunnel group hooks."""
        d = self.definition
        ident, count, prefix = d['tunnel_id'], d.get('count', 1), d.get('prefix16', '10.254')
        header = 'instance index key udp_port client_if server_if client_ipv4 server_ipv4 client_ipv6 server_ipv6 mark mask table chain client_port server_port client_label server_label client_socket server_socket client_has_tun server_has_tun no_address'
        rows = [header.split()]
        for index in range(count):
            suffix = f'_{index}' if index else ''
            member, key, h = f'{ident}{suffix}', ident + 256*index, 4*index+1
            ips = [f'{prefix}.{ident}.{h}', f'{prefix}.{ident}.{h+1}',
                   f'fd42::{prefix.replace(".", ":")}:{ident}:{h:x}',
                   f'fd42::{prefix.replace(".", ":")}:{ident}:{h+1:x}']
            if d.get('no_address'): ips = ['', '', '', '']
            sides = [d[s] for s in ('client', 'server')]
            rows.append([member, index, key, 40000+key, f'ut{member}c', f'ut{member}s', *ips,
                         d.get('mark', key<<16), d.get('mask', 0xffff0000), d.get('table', 10000+key),
                         d.get('chain', f'TUNTOM_{ident}')+suffix,
                         *[s.get('port_id', '')+suffix if s.get('switch_socket') else '' for s in sides],
                         *[s.get('label', '') for s in sides], *[s.get('switch_socket', '') for s in sides],
                         *[int(not s.get('switch_socket') or s.get('exit_node', False)) for s in sides],
                         int(d.get('no_address', False))])
        return ''.join('\t'.join(map(str, row))+'\n' for row in rows)


def json_text(value):
    return json.dumps(value, indent=2, sort_keys=True) + '\n'

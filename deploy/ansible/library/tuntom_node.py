#!/usr/bin/python3
"""Instance ownership and inspection; Ansible transports this module privately."""
from ansible.module_utils.basic import AnsibleModule
from ansible.module_utils.tuntom_node import perform


def main():
    module = AnsibleModule(argument_spec={
        'action': {'type': 'str', 'required': True},
        'payload': {'type': 'dict', 'required': True},
    }, supports_check_mode=True)
    action = module.params['action']
    readonly = action in ('inspect', 'recheck', 'preflight', 'assert-free')
    if module.check_mode and not readonly:
        module.exit_json(changed=False, skipped=True)
    try:
        result = perform(action, module.params['payload'])
        module.exit_json(changed=not readonly, report=result)
    except Exception as error:
        module.fail_json(msg=str(error))


if __name__ == '__main__':
    main()

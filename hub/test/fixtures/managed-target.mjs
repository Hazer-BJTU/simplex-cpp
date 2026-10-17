/** Test-only target emulator: it never opens the requested allocation paths. */
import { existsSync, readFileSync, writeFileSync, appendFileSync, unlinkSync } from 'node:fs';
import { join } from 'node:path';
import { parse } from 'yaml';

const root = process.env.MANAGED_FIXTURE_ROOT;
const args = process.argv.slice(2);
const load = path => JSON.parse(readFileSync(path, 'utf8'));
const active = path => {
    if (!existsSync(path)) return false;
    try { process.kill(Number(readFileSync(path, 'utf8')), 0); return true; }
    catch { return false; }
};
if (args[0] === 'run') {
    const config = args[args.indexOf('--config') + 1];
    const id = config.split('/').at(-3);
    const lease = join(root, `${id}.lease`);
    writeFileSync(join(root, 'dispatched.pid'), String(process.pid));
    while (existsSync(join(root, 'hold-start'))) await new Promise(resolve => setTimeout(resolve, 10));
    // Match the real manager resolver: a retired allocation cannot be acquired.
    if (load(join(root, `${id}.json`)).status !== 'ready') process.exit(1);
    writeFileSync(lease, String(process.pid));
    writeFileSync(join(root, 'last-run.json'), JSON.stringify(args));
    const finish = () => { try { unlinkSync(lease); } catch {} process.exit(0); };
    process.on('SIGTERM', finish);
    process.on('SIGINT', finish);
    process.on('exit', () => { try { unlinkSync(lease); } catch {} });
    if (existsSync(join(root, 'protocol-worker'))) {
        const record = load(join(root, `${id}.json`));
        const config = parse(record.configuration);
        config.persistence.directory = record.directory;
        config.client.endpoint = record.refresh.client_endpoint;
        config.security ??= {};
        config.security.confirmation = { endpoint: record.refresh.confirmation_endpoint };
        config.hub_remote_call = { endpoint: record.refresh.tools_endpoint };
        process.env.SIMPLEX_FIXTURE_CONFIGURATION = JSON.stringify(config);
        await import('./subagent-worker.js');
    }
    else if (existsSync(join(root, 'exit-worker'))) setTimeout(finish, 50);
    else setInterval(() => {}, 1000);
} else {
    const request = JSON.parse(Buffer.from(args[2], 'base64').toString('utf8'));
    const path = join(root, `${request.allocation_id}.json`);
    const lease = join(root, `${request.allocation_id}.lease`);
    appendFileSync(join(root, 'operations.jsonl'), JSON.stringify(request) + '\n');
    const failure = (code, message = code) => { throw { code, message }; };
    let record = existsSync(path) ? load(path) : null;
    const effective = () => {
        const result = parse(record.configuration);
        result.persistence.directory = record.directory;
        result.client.endpoint = record.refresh.client_endpoint ?? result.client.endpoint;
        if (result.security?.confirmation && record.refresh.confirmation_endpoint) result.security.confirmation.endpoint = record.refresh.confirmation_endpoint;
        if (result.hub_remote_call && record.refresh.tools_endpoint) result.hub_remote_call.endpoint = record.refresh.tools_endpoint;
        return result;
    };
    const describe = () => ({ ...record, config_path: `${record.directory}/config/config.yaml`,
        ...(record.status === 'ready' ? { effective_configuration: effective() } : {}) });
    const save = () => writeFileSync(path, JSON.stringify(record));
    try {
        if (existsSync(join(root, `delay-${request.operation}`))) await new Promise(resolve => setTimeout(resolve, 150));
        if (existsSync(join(root, 'offline'))) process.exit(255);
        if (request.operation === 'clean_fork' && existsSync(join(root, 'busy-clean_fork'))) failure('busy');
        if (active(lease) && !['inspect', 'read_state'].includes(request.operation)) failure('busy');
        if (!record && !['prepare', 'clean_fork'].includes(request.operation)) failure('not_found');
        let result;
        switch (request.operation) {
            case 'clean_fork': {
                if (!record) {
                    const parent = load(join(root, `${request.source.allocation_id}.json`));
                    if (parent.status !== 'ready') failure('not_ready');
                    if (parent.startup?.lifecycle_id !== request.source.lifecycle_id
                        || parent.startup?.revision !== request.source.revision) failure('conflict');
                    const configuration = structuredClone(parent.startup.configuration);
                    configuration.persistence.restore = 'if_present';
                    configuration.client = { ...configuration.client, endpoint: 'ws://unconfigured.invalid/events' };
                    configuration.security ??= {};
                    configuration.security.confirmation ??= {};
                    configuration.hub_remote_call ??= {};
                    record = { allocation_id: request.allocation_id, directory: request.directory,
                        configuration: JSON.stringify(configuration), fork_source: request.source,
                        refresh: {}, status: 'ready', revision: 1 };
                }
                if (JSON.stringify(record.fork_source) !== JSON.stringify(request.source)) failure('conflict');
                if (record.status !== 'ready') failure('retired');
                save(); result = describe(); break;
            }
            case 'prepare':
                if (!record) record = { allocation_id: request.allocation_id, directory: request.directory,
                    configuration: request.configuration, refresh: request.refresh ?? {}, status: 'ready', revision: 1 };
                if (record.status !== 'ready') failure('retired');
                if (request.configuration !== undefined && request.configuration !== record.configuration) failure('conflict');
                if (request.refresh && JSON.stringify(request.refresh) !== JSON.stringify(record.refresh)) {
                    if (request.expected_revision !== record.revision) failure('conflict');
                    record.refresh = request.refresh;
                    record.revision++;
                }
                save(); result = describe(); break;
            case 'inspect': result = describe(); break;
            case 'capture_startup':
                if (request.expected_revision !== record.revision) failure('conflict');
                record.startup = { lifecycle_id: request.lifecycle_id, revision: record.revision, configuration: effective() };
                save(); result = describe(); break;
            case 'read_state':
                if (existsSync(join(root, 'invalid-state'))) failure('invalid_data');
                result = { backend: 'json', state: { text: 'x'.repeat(10 * 1024 * 1024), extra: 'preserved' }, readable: '# Full readable state\n' };
                break;
            case 'remove': record.status = 'removed'; save(); result = describe(); break;
            default: failure('invalid_request');
        }
        const drop = join(root, `drop-${request.operation}`);
        if (existsSync(drop)) { unlinkSync(drop); process.stdout.write('{'); process.exit(255); }
        process.stdout.write(JSON.stringify({ version: 1, request_id: request.request_id, ok: true, result }));
    } catch (error) {
        process.stdout.write(JSON.stringify({ version: 1, request_id: request.request_id, ok: false,
            error: { code: error.code ?? 'fixture_error', message: error.message ?? 'fixture failure' } }));
        process.exitCode = 1;
    }
}

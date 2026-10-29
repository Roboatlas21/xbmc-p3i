"""Persistent user folders with host-isolated application assignments."""
import hashlib
import fcntl
import json
import os
import tempfile

FILE_NAME = 'folders.json'
_UNCONFIGURED = 'unconfigured'


class Library(object):
    def __init__(self, folder, host=None):
        self.path = os.path.join(folder, FILE_NAME)
        self.host_key = self.host_identity(host)
        self._data = None

    @staticmethod
    def host_identity(host):
        """Use the same normalized host identity as Luna's artwork cache."""
        normalized = (host or '').strip().lower()
        if not normalized:
            return _UNCONFIGURED
        return hashlib.sha256(normalized.encode('utf-8')).hexdigest()[:20]

    def _legacy_host(self):
        """If possible, assign legacy folders to their last cached host."""
        try:
            with open(os.path.join(os.path.dirname(self.path), 'applist.json'),
                      encoding='utf-8') as stream:
                payload = json.load(stream)
            key = payload.get('host') if isinstance(payload, dict) else None
            if (isinstance(key, str) and len(key) == 20 and
                    all(char in '0123456789abcdef' for char in key)):
                return key
        except (OSError, ValueError, TypeError):
            pass
        return self.host_key

    def _load(self):
        if self._data is not None:
            return self._data

        data = {'folders': [], 'assign_by_host': {}}
        try:
            with open(self.path, encoding='utf-8') as stream:
                loaded = json.load(stream)
            if isinstance(loaded, dict):
                folders = loaded.get('folders')
                if isinstance(folders, list):
                    data['folders'] = [str(name) for name in folders]

                assignments = loaded.get('assign_by_host')
                if isinstance(assignments, dict):
                    data['assign_by_host'] = {
                        str(key): {str(app_id): str(folder)
                                   for app_id, folder in value.items()}
                        for key, value in assignments.items()
                        if isinstance(value, dict)
                    }
                else:
                    # Version-one assignments were keyed only by app ID.
                    # Migrate to the known cached server (if available) or
                    # the current host, never broadcast to every server.
                    legacy = loaded.get('assign')
                    if isinstance(legacy, dict) and legacy:
                        key = self._legacy_host()
                        data['assign_by_host'][key] = {
                            str(app_id): str(folder)
                            for app_id, folder in legacy.items()
                        }
        except (OSError, ValueError, TypeError):
            pass

        self._data = data
        return data

    def _assignments(self):
        return self._load()['assign_by_host'].setdefault(self.host_key, {})

    def _refresh(self):
        # Another Library instance may have updated a different host's folders.
        # Re-read before mutations instead of overwriting its newer assignments.
        self._data = None
        return self._load()

    def _save(self):
        data = self._load()
        path = None
        try:
            folder = os.path.dirname(self.path)
            os.makedirs(folder, exist_ok=True)
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                                             dir=folder, prefix='.folders.',
                                             delete=False) as stream:
                path = stream.name
                json.dump(data, stream, ensure_ascii=False, indent=1)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(path, self.path)
            return True
        except OSError:
            return False
        finally:
            if path is not None:
                try:
                    os.remove(path)
                except OSError:
                    pass

    def folders(self):
        return list(self._load()['folders'])

    def _mutate(self, modify):
        """Serialize multi-instance changes and refresh disk state under lock.

        Folder names are global while each host's app assignments are scoped.
        A previously loaded Library must not overwrite assignments made by
        another Kodi invocation while it was alive.
        """
        folder = os.path.dirname(self.path)
        try:
            os.makedirs(folder, exist_ok=True)
            # Never delete the lock file: all processes must lock one inode.
            with open(self.path + '.lock', 'a+') as lock:
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX)
                try:
                    self._data = None
                    data = self._load()
                    if not modify(data):
                        return False
                    saved = self._save()
                    if not saved:
                        self._data = None
                    return saved
                finally:
                    fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        except OSError:
            self._data = None
            return False

    def add_folder(self, name):
        name = (name or '').strip()
        if not name:
            return False
        def modify(data):
            if name in data['folders']:
                return False
            data['folders'].append(name)
            return True
        return self._mutate(modify)

    def rename_folder(self, old, new):
        new = (new or '').strip()
        if not new:
            return False
        def modify(data):
            if old not in data['folders'] or new in data['folders']:
                return False
            data['folders'][data['folders'].index(old)] = new
            for assignments in data['assign_by_host'].values():
                for app_id, folder in assignments.items():
                    if folder == old:
                        assignments[app_id] = new
            return True
        return self._mutate(modify)

    def remove_folder(self, name):
        def modify(data):
            if name not in data['folders']:
                return False
            data['folders'].remove(name)
            for key, assignments in data['assign_by_host'].items():
                data['assign_by_host'][key] = {
                    app_id: folder for app_id, folder in assignments.items()
                    if folder != name
                }
            return True
        return self._mutate(modify)

    def folder_of(self, app_id):
        data = self._load()
        folder = self._assignments().get(str(app_id))
        return folder if folder in data['folders'] else None

    def assign(self, app_id, folder):
        app_id = str(app_id)
        def modify(data):
            assignments = data['assign_by_host'].setdefault(self.host_key, {})
            if folder is None:
                assignments.pop(app_id, None)
            else:
                if folder not in data['folders']:
                    return False
                assignments[app_id] = folder
            return True
        return self._mutate(modify)

    def split(self, apps):
        data = self._load()
        buckets = {name: [] for name in data['folders']}
        loose = []
        assignments = self._assignments()

        for app in apps:
            folder = assignments.get(str(app[0]))
            if folder not in buckets:
                loose.append(app)
            else:
                buckets[folder].append(app)

        return [(name, buckets[name]) for name in data['folders']], loose

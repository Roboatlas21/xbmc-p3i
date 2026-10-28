import json
import os

FILE_NAME = 'folders.json'


class Library(object):
    def __init__(self, folder):
        self.path = os.path.join(folder, FILE_NAME)
        self._data = None

    def _load(self):
        if self._data is not None:
            return self._data

        data = {'folders': [], 'assign': {}}
        try:
            with open(self.path, encoding='utf-8') as f:
                loaded = json.load(f)
            if isinstance(loaded, dict):
                folders = loaded.get('folders')
                assign = loaded.get('assign')
                if isinstance(folders, list):
                    data['folders'] = [str(x) for x in folders]
                if isinstance(assign, dict):
                    data['assign'] = {str(k): str(v) for k, v in assign.items()}
        except (OSError, ValueError, TypeError):
            pass

        self._data = data
        return data

    def _save(self):
        data = self._load()
        try:
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(self.path, 'w', encoding='utf-8') as f:
                json.dump(data, f, ensure_ascii=False, indent=1)
            return True
        except OSError:
            return False

    def folders(self):
        return list(self._load()['folders'])

    def add_folder(self, name):
        name = (name or '').strip()
        if not name:
            return False
        data = self._load()
        if name in data['folders']:
            return False
        data['folders'].append(name)
        return self._save()

    def rename_folder(self, old, new):
        new = (new or '').strip()
        data = self._load()
        if not new or old not in data['folders'] or new in data['folders']:
            return False
        data['folders'][data['folders'].index(old)] = new
        for app_id, folder in data['assign'].items():
            if folder == old:
                data['assign'][app_id] = new
        return self._save()

    def remove_folder(self, name):
        data = self._load()
        if name not in data['folders']:
            return False
        data['folders'].remove(name)
        data['assign'] = {k: v for k, v in data['assign'].items() if v != name}
        return self._save()

    def folder_of(self, app_id):
        data = self._load()
        folder = data['assign'].get(str(app_id))
        return folder if folder in data['folders'] else None

    def assign(self, app_id, folder):
        data = self._load()
        app_id = str(app_id)
        if folder is None:
            data['assign'].pop(app_id, None)
        else:
            if folder not in data['folders']:
                return False
            data['assign'][app_id] = folder
        return self._save()

    def split(self, apps):
        data = self._load()
        buckets = {name: [] for name in data['folders']}
        loose = []

        for app in apps:
            folder = self.folder_of(app[0])
            if folder is None:
                loose.append(app)
            else:
                buckets[folder].append(app)

        return [(name, buckets[name]) for name in data['folders']], loose

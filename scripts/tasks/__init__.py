# Setup based on docs from invoke: https://docs.pyinvoke.org/en/2.2/concepts/namespaces.html#importing-modules-as-collections

from invoke import Collection

from . import local
from . import remote 
from . import gcloud

ns = Collection()
ns.add_collection(gcloud)
ns.add_collection(local)
ns.add_collection(remote)
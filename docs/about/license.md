# License

fastcached is licensed under the [Apache License 2.0][apache]. Each
source file carries an SPDX-License-Identifier comment.

[apache]: https://www.apache.org/licenses/LICENSE-2.0

## Acknowledgements

fastcached re-implements the wire protocols defined by the
[memcached project](https://memcached.org/) (Brad Fitzpatrick et al.,
BSD-3-Clause) and the [Redis project](https://redis.io/) (Salvatore
Sanfilippo et al., dual-licensed under RSAL / SSPL).

The protocols themselves are the work of those projects; this codebase
is a compatible re-implementation that aims to interoperate with the
clients written against them. The wire formats, semantics, and command
sets follow the published specifications.

Trademarks of third parties remain the property of their respective
owners.

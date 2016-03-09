-------------------------------------------------------------------------------
                            Package `box.session`
-------------------------------------------------------------------------------

The ``box.session`` package allows querying the session state, writing to a
session-specific temporary Lua table, or setting up triggers which will fire
when a session starts or ends. A *session* is an object associated with each
client connection.

.. module:: box.session

.. function:: id()

    :return: the unique identifier (ID) for the current session.
             The result can be 0 meaning there is no session.
    :rtype:  number

.. function:: exists(id)

    :return: 1 if the session exists, 0 if the session does not exist.
    :rtype:  number

.. function:: peer(id)

    This function works only if there is a peer, that is,
    if a connection has been made to a separate server.

    :return: The host address and port of the session peer,
             for example "127.0.0.1:55457".
             If the session exists but there is no connection to a
             separate server, the return is null.
             The command is executed on the server,
             so the "local name" is the server's host
             and administrative port, and the "peer name" is the client's host
             and port.
    :rtype:  string

    Possible errors: 'session.peer(): session does not exist'

.. function:: sync()

    :return: the value of the :code:`sync` integer constant used in the
             `binary protocol
             <https://github.com/tarantool/tarantool/blob/1.6/src/box/iproto_constants.h>`_.

    :rtype:  number

.. data:: storage

    A Lua table that can hold arbitrary unordered session-specific
    names and values, which will last until the session ends.

=================================================
                      Example
=================================================

.. code-block:: tarantoolsession

    tarantool> box.session.peer(box.session.id())
    ---
    - 127.0.0.1:45129
    ...
    tarantool> box.session.storage.random_memorandum = "Don't forget the eggs"
    ---
    ...
    tarantool> box.session.storage.radius_of_mars = 3396
    ---
    ...
    tarantool> m = ''
    ---
    ...
    tarantool> for k, v in pairs(box.session.storage) do
             >   m = m .. k .. '='.. v .. ' '
             > end
    ---
    ...
    tarantool> m
    ---
    - 'radius_of_mars=3396 random_memorandum=Don''t forget the eggs. '
    ...

See the section :ref:`Triggers <box-triggers>`
for instructions about defining triggers for connect and disconnect
events with ``box.session.on_connect()`` and ``box.session.on_disconnect()``.
See the section :ref:`Access control <box-authentication>`
for instructions about ``box.session`` functions that affect user
identification and security.

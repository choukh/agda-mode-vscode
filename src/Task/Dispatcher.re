open Belt;

module Impl = (Editor: Sig.Editor) => {
  module ErrorHandler = Handle__Error.Impl(Editor);
  module GoalHandler = Handle__Goal.Impl(Editor);
  module CommandHandler = Handle__Command.Impl(Editor);
  module ResponseHandler = Handle__Response.Impl(Editor);
  module Task = Task.Impl(Editor);
  open! Task;

  type source =
    | Agda
    | View
    | Command
    | Misc;

  type status =
    | Busy
    | Idle;

  module MultiQueue = {
    //  This module represents a list of Task queues, tagged by some `source`,
    //  which is responsible for generating and pushing Tasks into its Task queue
    //
    //  For example, the initial queues should look something like this,
    //  with `Command` as the only source of Tasks
    //
    //    [ Command : [ Task0, Task1 ]
    //    ]
    //
    //  More Tasks will be added to this queue as the user gives more commands
    //
    //    [ Command : [ Task0, Task1, Task2, Task3 ]
    //    ]
    //
    //  On the other hand, Tasks are comsumed from the other end (that's how queue works, right?)
    //
    //    [ Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  However, some Tasks are special, they block other Tasks coming after them,
    //  and even generates Tasks on their own!
    //  `SendRequest(..)` (to Agda) is among one of these Tasks:
    //
    //    [ Agda    : [ Task4, Task5, Task6, .... ]
    //    , Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  Tasks of Reponses from Agda will be added to this new "Agda" queue.
    //  Tasks in the "Agda" queue have higher priority than those of the "Command" queue.
    //
    //    [ Agda    : [ Task5, Task6, Task7, ... ]
    //    , Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  Often, tasks in the "Agda" queue are consumed faster than they are generated by Agda.
    //  And this would cause the "Agda" queue to be empty.
    //  However, this doesn't mean that we can continue to consume Tasks in the "Command" queue,
    //  as more Tasks may will be added to the "Agda" queue.
    //
    //    [ Agda    : [ ]
    //    , Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  The "Agda" queue can only be removed after the request-response cycle is completed.
    //  Tasks in the "Command" queue can then continue to be executed afterwards.
    //
    //    [ Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  If there are any remaining Tasks in the "Agda" queue after the cycle is completed:
    //
    //    [ Agda    : [ Task5, Task6, Task7 ]
    //    , Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  The remaining Tasks should be prepended to the next queue with lower priority
    //  (in this case, the "Command" queue)
    //
    //    [ Command : [ Task5, Task6, Task7, Task1, Task2, Task3 ]
    //    ]
    //
    //  Note that other Tasks in the "Agda" queue may also behave like `SendRequest(..)`.
    //  For example, it may prompt an input box and wait until the user to reply.
    //  In this case, the "Agda" queue is now blocked by the "View" queue.
    //
    //    [ View    : [ Task8 ]
    //    , Agda    : [ Task5, Task6, Task7, ... ]
    //    , Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  Interestingly, the Tasks in the "Agda" queue may just be `SendRequest(..)`.
    //  Since there can only be one request to Agda at a time, the resouce is now occupied,
    //  The whole queues should be blocked, and wait until the current
    //  request-response cycle is completed.
    //
    //    [ Agda    : [ SendRequest(..), Task4 ]       (assume completed)
    //    , Command : [ Task1, Task2, Task3 ]
    //    ]
    //
    //  Then, the "Agda" queue should be removed while
    //  the remaining Tasks get concatenated to the next queue
    //
    //    [ Command : [ SendRequest(..), Task4, Task1, Task2, Task3 ]
    //    ]
    //
    //  And `SendRequest(..)` should be able to be executed and spawn a new "Agda" queue
    //
    //    [ Agda    : [ Task5, Task6, Task7, ... ]
    //    , Command : [ Task4, Task1, Task2, Task3 ]
    //    ]
    //
    type t = list((source, list(Task.t)));

    let make = () => [(Command, [])];

    let spawn = (queues, source) => {
      [(source, []), ...queues];
    };

    let remove = (queues, target) => {
      // walk through and remove the first matched queue (while leaving the rest)
      let lastQueueMatched = ref(None);
      queues->List.keepMap(((source, queue)) =>
        if (source == target && Option.isNone(lastQueueMatched^)) {
          lastQueueMatched := Some(queue);
          None; // this queue is to be removed
        } else {
          switch (lastQueueMatched^) {
          | Some(queue') =>
            // the previous queue was removed
            // should prepend to this queue
            lastQueueMatched := None;
            Some((source, List.concat(queue', queue)));
          | None => Some((source, queue)) // the boring case
          };
        }
      );
    };

    let addTasks = (queues, target, tasks) => {
      // walk through and concatenate the Tasks to the first matching queue
      let concatenated = ref(false);
      queues->List.keepMap(((source, queue)) =>
        if (source == target && ! concatenated^) {
          concatenated := true;
          Some((source, List.concat(queue, tasks)));
        } else {
          Some((source, queue));
        }
      );
    };

    let countBySource = (queues, target) =>
      queues->List.reduce(0, (accum, (source, _queue)) =>
        if (source == target) {
          accum + 1;
        } else {
          accum;
        }
      );

    let log = queues => {
      let strings =
        queues
        ->List.map(
            fun
            | (Agda, queue) =>
              "Agda " ++ Util.Pretty.list(List.map(queue, Task.toString))
            | (View, queue) =>
              "View " ++ Util.Pretty.list(List.map(queue, Task.toString))
            | (Command, queue) =>
              "Comm " ++ Util.Pretty.list(List.map(queue, Task.toString))
            | (Misc, queue) =>
              "Misc " ++ Util.Pretty.list(List.map(queue, Task.toString)),
          )
        ->List.toArray;
      Js.log(Js.Array.joinWith("\n", strings));
    };

    let rec getNextTask = (blocking, queues) =>
      switch (queues) {
      | [] => None // should not happen, the "Command" queue is gone
      | [(_source, []), ...queues] =>
        if (blocking) {
          None; // stuck waiting for the `_source`
        } else {
          getNextTask(blocking, queues)
          ->Option.map(((task, queues)) =>
              (task, [(_source, []), ...queues])
            );
        }
      | [(source, [task, ...queue]), ...queues] =>
        Some((task, [(source, queue), ...queues]))
      };

    let taskSize = queues =>
      queues->List.reduce(0, (accum, (_, queue)) =>
        accum + List.length(queue)
      );
  };

  type t = {
    mutable blocking: MultiQueue.t,
    mutable critical: MultiQueue.t,
    // status will be set to `Busy` if there are Tasks being executed
    // A semaphore to make sure that only one `kickStart` is running at a time
    mutable statusBlocking: status,
    mutable statusCritical: status,
  };

  let make = () => {
    blocking: MultiQueue.make(),
    critical: MultiQueue.make(),
    statusBlocking: Idle,
    statusCritical: Idle,
  };

  module Blocking = {
    let spawn = (self, target) =>
      self.blocking = MultiQueue.spawn(self.blocking, target);
    let remove = (self, target) =>
      self.blocking = MultiQueue.remove(self.blocking, target);
    let addTasks = (self, target, tasks) =>
      self.blocking = MultiQueue.addTasks(self.blocking, target, tasks);
    let countBySource = (self, target) =>
      MultiQueue.countBySource(self.blocking, target);
    let logQueues = self => MultiQueue.log(self.blocking);
    let getNextTask = self => MultiQueue.getNextTask(true, self.blocking);

    let addMiscTasks = (self, tasks) => {
      spawn(self, Misc);
      addTasks(self, Misc, tasks);
      remove(self, Misc);
      Promise.resolved(true);
    };
  };

  module Critical = {
    let spawn = (self, target) =>
      self.critical = MultiQueue.spawn(self.critical, target);
    let remove = (self, target) =>
      self.critical = MultiQueue.remove(self.critical, target);
    let addTasks = (self, target, tasks) =>
      self.critical = MultiQueue.addTasks(self.critical, target, tasks);
    let countBySource = (self, target) =>
      MultiQueue.countBySource(self.critical, target);
    let logQueues = self => MultiQueue.log(self.critical);
    let getNextTask = self => MultiQueue.getNextTask(false, self.critical);

    let addMiscTasks = (self, tasks) => {
      spawn(self, Misc);
      addTasks(self, Misc, tasks);
      remove(self, Misc);
      Promise.resolved(true);
    };
  };

  let sendAgdaRequest = (runTasks, state, req) => {
    // this promise get resolved after the request to Agda is completed
    let (promise, resolve) = Promise.pending();
    let handle = ref(None);
    let handler: result(Connection.response, Connection.Error.t) => unit =
      fun
      | Error(error) => {
          let tasks = ErrorHandler.handle(Error.Connection(error));
          runTasks(tasks);
        }
      | Ok(Parser.Incr.Event.Yield(Error(error))) => {
          let tasks = ErrorHandler.handle(Error.Parser(error));
          runTasks(tasks);
        }
      | Ok(Yield(Ok(response))) => {
          Js.log(">>> " ++ Response.toString(response));
          let tasks = ResponseHandler.handle(response);
          runTasks(tasks);
        }
      | Ok(Stop) => {
          Js.log(">>| ");
          resolve();
        };

    state
    ->State.sendRequestToAgda(req)
    ->Promise.flatMap(
        fun
        | Ok(connection) => {
            handle := Some(connection.Connection.emitter.on(handler));
            promise;
          }
        | Error(error) => {
            let tasks = ErrorHandler.handle(error);
            runTasks(tasks);
            promise;
          },
      )
    ->Promise.tap(() => (handle^)->Option.forEach(f => f()));
  };

  let isCritical =
    fun
    | Command.Escape => true
    | InputSymbol(_) => true
    | _ => false;

  let rec executeTask = (self, state: State.t, task) => {
    Js.log("\n\nTask: " ++ Task.toString(task));
    Critical.logQueues(self);
    Js.log("-------------------------------");
    Blocking.logQueues(self);
    switch (task) {
    | DispatchCommand(command) =>
      let tasks = CommandHandler.handle(command);
      Critical.addTasks(self, Command, tasks);
      Promise.resolved(true);
    | SendRequest(request) =>
      if (Blocking.countBySource(self, Agda) > 0) {
        // there can only be 1 Agda request at a time
        Promise.resolved(false);
      } else {
        Blocking.spawn(self, Agda);
        sendAgdaRequest(
          tasks => {
            Blocking.logQueues(self);
            Blocking.addTasks(self, Agda, tasks);
            kickStart(self, state);
          },
          state,
          request,
        )
        ->Promise.get(() => {Blocking.remove(self, Agda)});
        // NOTE: return early before `sendAgdaRequest` resolved
        Promise.resolved(true);
      }
    | ViewReq(View.Request.Plain(header, Query(x, y)), callback) =>
      let request = View.Request.Plain(header, Query(x, y));
      if (Blocking.countBySource(self, View) > 0) {
        // there can only be 1 View request at a time (NOTE, revise this)
        Promise.resolved(
          false,
        );
      } else {
        Blocking.spawn(self, View);
        state
        ->State.sendRequestToView(request)
        ->Promise.map(response => {
            Blocking.addTasks(self, View, callback(response))
          })
        ->Promise.map(() => {
            Blocking.remove(self, View);
            true;
          });
      };
    | ViewReq(request, callback) =>
      Critical.spawn(self, View);
      state
      ->State.sendRequestToView(request)
      ->Promise.map(response => {
          Critical.addTasks(self, View, callback(response))
        })
      ->Promise.map(() => {
          Critical.remove(self, View);
          true;
        });
    | WithState(callback) =>
      Blocking.spawn(self, Misc);
      callback(state)
      ->Promise.map(Blocking.addTasks(self, Misc))
      ->Promise.tap(() => {Blocking.remove(self, Misc)})
      ->Promise.map(() => true);
    | Terminate => State.destroy(state)->Promise.map(() => false)
    | Goal(action) =>
      let tasks = GoalHandler.handle(action);
      Blocking.addMiscTasks(self, tasks);
    | ViewEvent(event) =>
      let tasks =
        switch (event) {
        | Initialized => []
        | Destroyed => [Task.Terminate]
        };
      Critical.addMiscTasks(self, tasks);
    | Error(error) =>
      let tasks = ErrorHandler.handle(error);
      Critical.addMiscTasks(self, tasks);
    | Debug(message) =>
      Js.log("DEBUG " ++ message);
      Promise.resolved(true);
    };
  }
  // consuming Tasks in the `queues`
  and kickStart = (self, state) => {
    switch (self.statusCritical) {
    | Busy => ()
    | Idle =>
      switch (Critical.getNextTask(self)) {
      | None => ()
      | Some((task, queues)) =>
        self.critical = queues;
        self.statusCritical = Busy;
        executeTask(self, state, task) // and start executing tasks
        ->Promise.get(keepRunning => {
            self.statusCritical = Idle; // flip the semaphore back
            if (keepRunning) {
              // and keep running
              kickStart(self, state);
            };
          });
      }
    };

    switch (self.statusBlocking) {
    | Busy => ()
    | Idle =>
      switch (Blocking.getNextTask(self)) {
      | None => ()
      | Some((task, queues)) =>
        self.blocking = queues;
        self.statusBlocking = Busy;
        executeTask(self, state, task) // and start executing tasks
        ->Promise.get(keepRunning => {
            self.statusBlocking = Idle; // flip the semaphore back
            if (keepRunning) {
              // and keep running
              kickStart(self, state);
            };
          });
      }
    };
  };

  let dispatchCommand = (self, state: State.t, command) => {
    Critical.addTasks(self, Command, [DispatchCommand(command)]);
    kickStart(self, state);
  };

  let destroy = _ => ();
};
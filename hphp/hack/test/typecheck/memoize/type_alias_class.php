<?hh // strict

type MyBar = Bar;

class Bar implements IMemoizeParam {
  public function getInstanceKey(): string {
    return "dummy";
  }
}

class Foo {
  <<__Memoize>>
  public async function someMethod(MyBar $arg): Awaitable<string> {
    return "hello";
  }
}

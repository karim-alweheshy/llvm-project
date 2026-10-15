public protocol HasValue {
  var integer: Int { get }
}

public struct Payload: HasValue {
  public let integer: Int = 23
  public let flag: Bool = true
}

public final class Holder {
  public let payload = Payload()
  public init() {}
}

public func makePayload() -> Payload { Payload() }
public func makeOpaque() -> some HasValue { Payload() }

@_cdecl("reflection_probe_marker")
public func marker() -> Int32 { 7 }

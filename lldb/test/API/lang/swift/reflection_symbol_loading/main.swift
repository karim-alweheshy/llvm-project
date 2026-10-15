import Foundation
import ReflectionProbe

@inline(never)
func exercise() {
  let payload = makePayload()
  let holder = Holder()
  let opaque = makeOpaque()
  let number = NSNumber(value: 7)
  let text = NSString(string: "small")
  print(payload, holder, opaque, number, text) // Set breakpoint here.
}

exercise()

import Foundation
import Mapped

#if PARTIAL_MODULE_MAP
import CShim
#endif

func main() {
  let c = MappedValue()
  let date = Date(timeIntervalSince1970: 0)
#if PARTIAL_MODULE_MAP
  let shim = cshim_value()
#endif
  print(c, date) // Set breakpoint here
#if PARTIAL_MODULE_MAP
  print(shim)
#endif
}

main()

<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group abstract="true" id="twice">
    <sch:rule context="$element">
      <sch:report test="true()">First rule.</sch:report>
    </sch:rule>
    <sch:rule context="$element">
      <sch:report test="true()">Second rule.</sch:report>
    </sch:rule>
  </sch:group>
  <sch:group is-a="twice">
    <sch:param name="element" value="table"/>
  </sch:group>
</sch:schema>

<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:group>
    <sch:rule context="/">
      <sch:report test="true()">Root.</sch:report>
    </sch:rule>
    <sch:rule context="node()">
      <sch:report test="true()">Node <sch:value-of select="name()"/>.</sch:report>
    </sch:rule>
    <sch:rule context="@*">
      <sch:report test="true()">Attribute.</sch:report>
    </sch:rule>
    <sch:rule context="comment()">
      <sch:report test="true()">Comment.</sch:report>
    </sch:rule>
    <sch:rule context="processing-instruction()">
      <sch:report test="true()">Instruction.</sch:report>
    </sch:rule>
  </sch:group>
</sch:schema>

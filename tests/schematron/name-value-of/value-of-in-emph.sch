<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3">
  <sch:pattern>
    <sch:rule context="item[@n = 1]">
      <sch:report test="true()"><sch:emph><sch:name/></sch:emph> <sch:span class="v"><sch:value-of select="@n"/></sch:span> <sch:dir value="rtl"><sch:value-of select="@n + 1"/></sch:dir></sch:report>
    </sch:rule>
  </sch:pattern>
</sch:schema>

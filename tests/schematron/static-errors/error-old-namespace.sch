<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://www.ascc.net/xml/schematron">
  <sch:pattern name="p">
    <sch:rule context="item">
      <sch:assert test="@n">An item has an n attribute.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>

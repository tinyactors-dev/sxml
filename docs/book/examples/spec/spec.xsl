<xsl:package name="spec.xsl" package-version="1.0" version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:spec="urn:example:spec">

  <!-- Private: every element with an anchor, found by its anchor. -->
  <xsl:key name="anchor" match="*[@anchor]" use="@anchor"/>

  <!-- Private: the strength of each BCP 14 key word. -->
  <xsl:variable name="spec:strengths" as="map(xs:string, xs:string)" select="{
      'MUST': 'absolute', 'MUST NOT': 'absolute', 'REQUIRED': 'absolute',
      'SHALL': 'absolute', 'SHALL NOT': 'absolute',
      'SHOULD': 'recommended', 'SHOULD NOT': 'recommended',
      'RECOMMENDED': 'recommended', 'NOT RECOMMENDED': 'recommended',
      'MAY': 'optional', 'OPTIONAL': 'optional' }"/>

  <xsl:function name="spec:strength" as="xs:string" visibility="public">
    <xsl:param name="keyword" as="element(bcp14)"/>
    <xsl:sequence select="$spec:strengths(normalize-space($keyword))"/>
  </xsl:function>

  <!-- "3.1" for a section, "R6" for a requirement: numbers by position. -->
  <xsl:function name="spec:number" as="xs:string" visibility="public">
    <xsl:param name="node" as="element()"/>
    <!-- One text node, whose string is the result. -->
    <xsl:value-of>
      <xsl:choose>
        <xsl:when test="$node/self::section">
          <xsl:number select="$node" level="multiple" count="section" format="1.1"/>
        </xsl:when>
        <xsl:otherwise>
          <xsl:text>R</xsl:text>
          <xsl:number select="$node" level="any" count="t[@anchor]"/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:value-of>
  </xsl:function>

  <!-- The text of an xref: its own, or one made from what it points to. -->
  <xsl:function name="spec:xref-text" as="xs:string" visibility="public">
    <xsl:param name="xref" as="element(xref)"/>
    <xsl:variable name="target" select="key('anchor', $xref/@target, root($xref))"/>
    <xsl:sequence select="
        if ($xref/node()) then string($xref)
        else if ($target/self::section) then 'Section ' || spec:number($target)
        else if ($target/self::t) then 'Requirement ' || spec:number($target)
        else '[' || $xref/@target || ']'"/>
  </xsl:function>
</xsl:package>

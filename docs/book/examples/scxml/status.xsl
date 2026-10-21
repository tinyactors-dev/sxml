<!-- The status a lock in a given state answers with, made from the
     machine's data and validated as it is made. -->
<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:sc="http://www.w3.org/2005/07/scxml"
    xmlns:latch="urn:example:latch"
    exclude-result-prefixes="sc">
  <xsl:import-schema namespace="http://www.w3.org/2005/07/scxml"
                     schema-location="scxml.xsd"/>
  <xsl:output method="xml" indent="yes"/>

  <!-- The parameters are text; the status made from them is checked. -->
  <xsl:param name="door" select="'front'"/>
  <xsl:param name="state" select="'locked'"/>
  <xsl:param name="attempts" select="0"/>

  <!-- Validate the input, so that its values have the schema's types. -->
  <xsl:template match="/">
    <xsl:variable name="machine" as="document-node(schema-element(sc:scxml))">
      <xsl:copy-of select="." validation="strict"/>
    </xsl:variable>
    <xsl:apply-templates select="$machine/sc:scxml"/>
  </xsl:template>

  <!-- The new status is validated against its declaration as it is made. -->
  <xsl:template match="schema-element(sc:scxml)">
    <latch:status xsl:validation="strict">
      <latch:door>{$door}</latch:door>
      <latch:state>{$state}</latch:state>
      <latch:failed-attempts>{$attempts}</latch:failed-attempts>
      <xsl:if test="$state = 'lockout'">
        <latch:retry-after>{sc:datamodel/sc:data[@id = 'lockoutSeconds']/@expr}</latch:retry-after>
      </xsl:if>
    </latch:status>
  </xsl:template>
</xsl:stylesheet>
